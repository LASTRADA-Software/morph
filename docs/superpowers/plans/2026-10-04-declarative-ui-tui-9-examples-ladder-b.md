# Declarative UI, Part 9 — the ladder rungs kanban, ledger and lims Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Re-express the kanban, ledger and lims clients as toolkit-free app libraries (controllers plus `ui::Node`
views) with one binary each that picks the TUI or Qt Quick frontend at runtime, and delete their QML, bridges,
presenters and Qt mains.

**Architecture:** Each rung gains `app/` (target `ladder_<rung>_app`: controllers built from `reactive::Query`,
`Mutation`, runtime `forms::FormSession`s and `Computed` projections; views that are bindings only) and
`ui/main.cpp` (the composition root of spec 4 §3). The domain libraries lose Qt: kanban's HTTP attachment server and
ledger's report-runner `App` move into `src/server/`, beside the servers that own them. Kanban keeps its headless
Qt-core test client, now driving the `BoardController` over `QtWebSocketBackend`.

**Tech Stack:** C++23; `morph::reactive`, `morph::ui`, the forms engine, `examples/common/app` (Part 6),
`morph::offline` (optional), POSIX sockets (kanban's TUI attachment client), Qt 6 only in servers, the headless
client and kanban's Qt attachment transfer; Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-10-04-examples-migration-design.md` (spec 4) §1–§3, §5 (kanban, ledger, lims),
§6, §7, §8, §9; `docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` (spec 1) §4b (the controller) and
§5 (drag-and-drop on `Common`: `dragKey`, `accepts`, `onDrop`).

This is **Part 9 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` commit convention every task below follows. This part has **three groups** — keys `kanban`, `ledger`,
`lims` — and each group ends by squashing its own `wip(<key>)` commits into one commit (commits 14, 15 and 16 of
the pull request's history).

## Global Constraints

Part 1's constraints apply unchanged. In addition:

- `app/` is toolkit-free: no Qt header, no `morph/qt*`, no `morph/tui`, no `morph/qt_quick` include. Controllers
  (`app/controllers/`) include nothing from `morph/ui/`; views (`app/views/`) are bindings only — every
  conditional, format and validation is a controller member or a `Computed` (spec 4 §7 rules 1–2).
- Client code lives in `namespace kanban::client`, `ledger::client`, `lims::client`; each app's factory is
  `[[nodiscard]] std::unique_ptr<ui::Application> <app>::client::makeApplication(ui::AppContext& ctx,
  examples::AppEnvironment const& env)` (the contract's "Conventions for every example application"). An app-specific
  knob comes from `env`, from an environment variable read through `ui::processEnvironment()`, or is defaulted, so
  every `ui/main.cpp` has spec 4 §3's shape.
- Controllers are built from `morph::examples::Wiring` (`"app/wiring.hpp"`), derive completions with
  `morph::examples::mapCompletion` (`"app/completion_map.hpp"`) and read a DTO's form with
  `morph::forms::FormModel::forAction<A>()` (Part 5) — none of the three is redefined per app.
- The domain libraries (`include/`, `src/` outside `src/server/`) include no Qt header; server-only Qt code lives in
  `src/server/app/` with its headers under `src/server/include/`, which Part 8's `morph_add_rung` builds into
  `ladder_<rung>_server_app` for the server and the tests.
- No identifier shorter than three characters outside `.clang-tidy`'s exceptions (parameters `i j k x y n N fn cb
  op`, variables `i j k x y lk cb op fn`): the runtime is `runtime`, never `rt`; an id is `taskId`, `rowId`, never
  `id` (struct members keep the DTOs' spelling). Headers under `app/` use `.at()` or iteration, never an unchecked
  container `operator[]`.
- No two adjacent parameters of interchangeable types (`bugprone-easily-swappable-parameters`): a call with two
  strings or two ids takes a request struct.
- Every `switch` over an enum names every enumerator **and** has a `default:` (`-Wswitch-enum` with
  `-Wswitch-default`, `cmake/compiler_options.cmake`).
- Tests never sleep. Controller tests wait with `morph::examples::testing::pumpUntil(owner, …)` (Part 6,
  `testkit/wait.hpp`) over a `MainThreadExecutor`; a test whose subject needs the Qt event loop (an
  `AttachmentServer`, a `QtWebSocketServer`, ledger's report runner) uses `morph::ladder::testkit::pumpUntil`
  (`testkit/pump.hpp`) over a `QtExecutor` owner, which spec 4 §4 keeps for exactly that case.
- New test cases are tagged `[<rung>][client]`, `[<rung>][view]` or `[<rung>][smoke]`. Controller and view tests live
  under `examples/<rung>/tests/client/` and run in `ladder_<rung>_tests`; frontend smoke tests live under
  `examples/<rung>/tests/smoke/` and run in `ladder_<rung>_smoke_tests` (Part 6: its `main` owns no Qt application
  object, because the Qt Quick frontend constructs its own).
- No `CHANGELOG.md` entry (CONTRIBUTING: examples-only changes get none). `docs/spec/` is touched only where a spec
  names a file this part deletes.
- Each group ends with its own squash; a group never contains another group's `wip` commit.

## Review Focus

1. **A drop whose key is not a task id** (a foreign drag, a string key) must be refused by every board drop target
   and never reach `MoveTaskPosition` (Task K12, test "a drop carrying a non-task key is refused and moves nothing").
2. **A move made while offline** must not be dispatched, must survive in the queue, and must replay exactly once on
   reconnect (Task K8, test "a move made while offline is queued, not sent, and replays on reconnect").
3. **Closing the board while polling** must leave no poll timer armed, so a closed board is never refreshed behind
   the user's back (Task K6, test "closing the board stops polling").
4. **A statement job that reached Done** must stop being polled and keep its lines (Task L6, test "a statement is
   polled to Done; a Done job stops the poll and keeps its lines").
5. **The sign-in reply on screen** must never carry the minted token (Task K3, test "the sign-in reply on screen
   carries no token").

## What this plan relies on from earlier parts

The interface contract fixes the names below, and the upstream plans fix their shapes; this section states the
shapes this plan's code is written against. If an upstream plan changes one, adapt the named call sites and state
it in the group's squash body (master plan, "The docs commit").

- **`morph_add_rung`, as Part 6 (C1–C3) and Part 8 (Task 1) write it.** `app/**/*.cpp` → STATIC
  `ladder_<rung>_app` with PUBLIC include dirs `app/` and `include/`, linking `morph::morph`,
  `morph::ladder_app_common` and `morph::ladder_<rung>_lib` — so `#include "controllers/x.hpp"` resolves under
  `<rung>/app/` and this plan adds no include or link line for the app library. `ui/*.cpp` → the executable `<rung>`
  (`morph_add_example_ui`, only when a frontend is built), with `MORPH_EXAMPLE_HAS_TUI` and
  `MORPH_EXAMPLE_HAS_QT_QUICK` always defined to `0` or `1`. `ladder_<rung>_tests` (`tests/**/*.cpp` minus
  `tests/smoke/`) links `morph::ladder_<rung>_app` and `morph::example_testkit`; `tests/smoke/*.cpp` →
  `ladder_<rung>_smoke_tests` (`morph_test_main`, both definitions). `src/server/app/*.cpp` → STATIC
  `ladder_<rung>_server_app` (natively, when such files exist) with PUBLIC include dir `src/server/include`, linking
  PUBLIC `morph::ladder_<rung>_lib morph::qt morph_qt_impl Qt6::Core` with `AUTOMOC ON`; the server and
  `ladder_<rung>_tests` link it, the app library and the binary do not, and the server no longer compiles
  `src/server/app/` itself. `ladder_<rung>_lib` links `Qt6::Core` (and turns on `AUTOMOC`) only while the rung has
  no `app/` directory.
- **Part 6 (`examples/common`, include root `examples/common`)** — `"app/app_environment.hpp"`
  (`morph::examples::AppEnvironment{server, db, user, seed, pollId}`; `fromArgs` reads `--server`, `--db`, `--user`,
  `--poll` and the `--seed` flag and ignores every other argument); `"app/transport.hpp"` (`Connection` with
  `bridge()` and `callbacks()` — `callbacks()` is `ctx.executor()` —, `LocalSetup{setupDatabase, workers}`,
  `connect(ctx, env, local)`, which sets `--user` as the default session when it is given, and `TransportError`);
  `"app/uuid.hpp"` (`newUuid()`); `"app/wiring.hpp"` (`morph::examples::Wiring{runtime, scheduler, bridge,
  callbacks}`, all four borrowed); `"app/completion_map.hpp"` (`morph::examples::mapCompletion<To>(owner, token,
  from, onValue, onError)`: both callbacks run on `owner`, gated by `token`; a throw from `onValue` fails the derived
  completion; `onError` observes a failure before it is passed on — Part 6 owns its tests); `"app/poller.hpp"`
  (`Poller<Event, Cursor>(Runtime&, Scheduler&, Fetch, Cursor start, OnEvent, PollerOptions{interval})`, `Fetch =
  std::function<async::Completion<PollPage<Event, Cursor>>(Cursor const&)>`, `PollPage{events, next}`, tracked
  `stoppedBy()`; it arms its timer through the given `Scheduler` and cancels it on destruction);
  `"testkit/wait.hpp"` (`morph::examples::testing::pumpUntil(MainThreadExecutor&, pred, budget)`);
  `"testkit/frontend_smoke.hpp"` (`runFrontendSmoke(factory, SmokeFrontend)`, which skips a frontend the configure
  did not build). The rig's Qt pump (`"testkit/pump.hpp"`, `morph::ladder::testkit::pumpUntil`, `awaitQt`) stays for
  tests whose subject needs the Qt event loop.
- **Part 5** — `<morph/forms/engine/field_model.hpp>` (`FormModel`, `FormModel::forAction<A>()`, which throws
  `std::logic_error` naming the schema path when the action's own schema does not read, `SubmitMode`),
  `<morph/forms/engine/form_session.hpp>` (`FormSession(Runtime&, FormModel, Submitter, ChoiceFetcher,
  FormSessionOptions = {})`, `Submitter`, `ChoiceFetcher`), `<morph/forms/engine/handler_submitter.hpp>`
  (`handlerSubmitter`/`handlerChoiceFetcher(callbacks, handlers...)`: each action goes to the first handler whose
  `servesAction` holds; an action none serves fails with `std::invalid_argument` naming it) and
  `<morph/forms/engine/form_view.hpp>` (`formView`, `FormViewOptions{.submitLabel}`). `pending()`, `lastReply()`
  and `lastError()` are tracked; `prefill(json)` **replaces every field** (a member the JSON does not name starts
  blank), `assign(path, jsonValue)` sets one field, and `reset()` blanks every field — all three programmatically,
  so none submits. So a controller fills a form's context (a project, a task, a client id) with `assign`, after any
  `reset`, and a test fills the user's fields one by one (`fill`, below), never with `prefill`, which would blank
  that context. `submit()` does nothing unless the form is ready, and otherwise runs the submission's `Mutation`, so
  `pending()` is true when it returns. Every action this plan renders declares `explicitSubmit`, so its form view
  carries a Submit button, labelled by `submitLabel`, whose `enabled` follows `ready()`; the view shows the last
  reply (`ok: …`) or error (`error: …`) itself. Part 5 has no success hook: a controller that acts on a reply wraps
  the `Submitter` it hands the session (`mapCompletion` over the handler's `executeJson`).
- **Part 2** — `<morph/ui/testing/recording_backend.hpp>` and `<morph/ui/mount.hpp>` (`Mounted(runtime, backend,
  root)`; the initial mount is synchronous). Kinds are the widget interfaces' names without `Widget`, except that a
  stack is `Column` or `Row` by its axis; props are the setters' names without `set`. Values are unquoted
  (`text=Hi bob`), a string `Key` is quoted and an integer one is not (`selected="Manager"`, `dragKey=7`). A helper
  (`click`, `edit`, `drag`, `pick`, …) calls the widget's callback synchronously, and does nothing to a widget whose
  own `enabled` (or `visible`) prop is `false`; the flush a callback causes is posted to the runtime's owner, so a
  test settles before it reads the tree — and before it clicks a button whose `enabled` a write just changed. A
  `Dialog`'s content exists only while it is open; a `Panel`'s child always exists; a `Tabs` page is mounted the
  first time its tab is selected and only hidden afterwards, so the tree of an app on its first tab holds the other
  tabs' labels (the `Tabs` widget's `tabs` prop) but none of their content.

---
## File Structure

### Group `kanban` (`examples/kanban/`)

| File | Responsibility |
|---|---|
| `src/server/include/kanban/http/attachment_server.hpp`, `src/server/app/attachment_server.cpp` | The HTTP attachment side channel (moved from `include/kanban/http/`, `src/http/`); in `ladder_kanban_server_app` (Part 8), server-only, Qt |
| `app/support/board_format.{hpp,cpp}` | Display strings: column headers, project lines, rule lines, activity, sync lines |
| `app/controllers/session_controller.{hpp,cpp}` | Sign-in form; installs the token; redacts it from the reply |
| `app/controllers/projects_controller.{hpp,cpp}` | Project list, create-project form, members (role picker, remove, add-member form) |
| `app/controllers/offline_config.hpp` | `OfflineConfig` (always built) |
| `app/controllers/offline_moves.{hpp,cpp}` | Durable offline queue, network monitor, reconnect replay (`MORPH_BUILD_OFFLINE_SQLITE`) |
| `app/controllers/board_controller.{hpp,cpp}` | Attach, board/activity queries, board forms, moves, shape and cards, polling, offline hook |
| `app/controllers/rules_controller.{hpp,cpp}` | Rules dialog: list, create form (with its Choice fetcher), delete |
| `app/attachments/attachment_transfer.{hpp,cpp}` | `IAttachmentTransfer`, request structs, `contentTypeFor`, `localPathFrom`, `attachmentServerFor` |
| `app/http/http_client.{hpp,cpp}` | Minimal blocking HTTP/1.1 client over POSIX sockets; URL and response parsing |
| `app/http/http_attachment_transfer.{hpp,cpp}` | `IAttachmentTransfer` over `http_client` on a worker thread |
| `app/controllers/task_detail_controller.{hpp,cpp}` | Task dialog: comments, add-comment form, attachments list, upload, download |
| `app/app_controller.{hpp,cpp}` | Owns the five controllers; the route `Computed` |
| `app/views/{sign_in,projects,board,root}_view.{hpp,cpp}` | `ui::Node` builders per screen, bindings only |
| `app/kanban_application.{hpp,cpp}` | `kanban::client::makeApplication`: connection, local token issuer, attachment transfer and offline queue from the environment |
| `ui/main.cpp` | Composition root (spec 4 §3) |
| `qt/qt_attachment_transfer.{hpp,cpp}` | `makeQtAttachmentTransfer`: `IAttachmentTransfer` over `QNetworkAccessManager`, behind a Qt-free header; compiled into `ladder_kanban_app` only with Qt Quick |
| `headless/main.cpp` | Headless Qt-core client driving `BoardController` (moved from `src/headless/`) |
| `CMakeLists.txt` | Server-side attachment wiring, headless target, Qt transfer, offline flags |
| `tests/client/*.cpp`, `tests/client/kanban_client_support.hpp` | Controller, transfer and view tests |
| `tests/smoke/test_kanban_frontends.cpp` | The app mounts and quits on each built frontend (`ladder_kanban_smoke_tests`) |
| `tests/test_board_concurrent_drag.cpp` | Rewritten over `BoardController` |
| `README.md` | Client section; visible differences |
| Deleted | `gui/`, `gui_lib/`, `src/headless/`, `include/kanban/http/`, `src/http/`, `tests/test_board_presenter.cpp`, `test_board_qml_bridge.cpp`, `test_board_offline_bridge.cpp`, `test_project_admin_presenter.cpp`, `test_project_admin_qml_bridge.cpp`, `test_gui_forms_render.cpp`, `test_board_layout.cpp`, `test_kanban_qml_surface.cpp`, `test_gui_qml_smoke.cpp` |

Outside the rung, in the kanban group: `codecov.yml` (kanban's ignore entries), `scripts/coverage.sh` (`app` among
the measured sub-directories), `docs/spec/offline/offline.md` and `examples/IMPLEMENTATION.md` (the one sentence
each that names `board_qml_bridge.cpp`).

### Group `ledger` (`examples/ledger/`)

| File | Responsibility |
|---|---|
| `src/server/include/ledger/app/app.hpp`, `src/server/app/app.cpp` | The report-runner `ledger::app::App` (moved from `include/ledger/app/`, `src/app/`); in `ladder_ledger_server_app` (Part 8), server-only, Qt |
| `app/support/ledger_format.{hpp,cpp}` | Account/entry/report rows, id and amount parsing, month check, local UTC offset |
| `app/controllers/boot_controller.{hpp,cpp}` | Dev principal (`--user`): local session or remote `Login`; optional seed ledger |
| `app/controllers/accounts_controller.{hpp,cpp}` | Accounts tab: ledger query, open account, store transfer, list month, undo |
| `app/controllers/budget_controller.{hpp,cpp}` | Budgets tab: category, link, budget, limit, report |
| `app/controllers/rules_controller.{hpp,cpp}` | Rules tab: create, update, last rule |
| `app/controllers/statement_controller.{hpp,cpp}` | Statement tab: submit, `Query` with `refreshEvery`, lines |
| `app/app_controller.{hpp,cpp}` | Owns boot and the four tab controllers |
| `app/views/ledger_views.{hpp,cpp}` | Boot screen and the four tabs |
| `app/ledger_application.{hpp,cpp}` | `ledger::client::makeApplication` (book 1 or `--seed`'s, the local UTC offset) |
| `ui/main.cpp` | Composition root (spec 4 §3) |
| `tests/client/*.cpp`, `tests/client/ledger_client_support.hpp` | Controller and view tests |
| `tests/smoke/test_ledger_frontends.cpp` | The app mounts and quits on each built frontend (`ladder_ledger_smoke_tests`) |
| `tests/test_multiclient.cpp` | Rewritten over `AccountsController` |
| `README.md` | Client section |
| Deleted | `gui/`, `gui_lib/`, `include/ledger/app/`, `src/app/`, `tests/test_ledger_presenter.cpp`, `test_ledger_qml_bridge.cpp`, `test_budget_presenter.cpp`, `test_budget_qml_bridge.cpp`, `test_rule_presenter.cpp`, `test_report_presenter.cpp`, `test_report_job_poller.cpp`, `test_ledger_qml_surface.cpp`, `test_gui_qml_smoke.cpp` |

Outside the rung: `codecov.yml` (ledger's ignore entries).

### Group `lims` (`examples/lims/`)

| File | Responsibility |
|---|---|
| `app/support/lims_format.{hpp,cpp}` | Sample header, result and conflict lines, quantity text |
| `app/controllers/sample_controller.{hpp,cpp}` | Lifecycle tab: register forms, open, transitions, return/reject forms |
| `app/controllers/result_controller.{hpp,cpp}` | Results tab: catalogue, attach, capture form, results, verify, conflicts, resolve form |
| `app/app_controller.{hpp,cpp}` | Owns both controllers |
| `app/views/lims_views.{hpp,cpp}` | The two tabs and the shell |
| `app/lims_application.{hpp,cpp}` | `lims::client::makeApplication` (`--db`, else `LIMS_DB`, else `lims.db`) |
| `ui/main.cpp` | Composition root (spec 4 §3) |
| `tests/client/*.cpp`, `tests/client/lims_client_support.hpp` | Controller and view tests |
| `tests/smoke/test_lims_frontends.cpp` | The app mounts and quits on each built frontend (`ladder_lims_smoke_tests`) |
| `README.md` | "The client" section rewritten |
| Deleted | `gui/`, `gui_lib/`, `tests/test_lims_presenters.cpp`, `test_lims_qml_bridges.cpp`, `test_lims_qml_surface.cpp`, `test_gui_qml_smoke.cpp` |

Outside the rung: `codecov.yml` (lims' ignore entries) and `examples/crm/include/crm/gui/crm_schemas.hpp` (its
comment names the deleted `lims_schemas.hpp`). `cmake/morph_add_rung.cmake` is not touched: Part 8 already links
`ladder_<rung>_lib` to Qt only while a rung has no `app/` (Task M7 confirms it).

## Build and test commands

```bash
# Once, before Task K1 (the master plan's build/all plus the optional offline queue kanban exercises):
cmake -S . -B build/all -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_BANK_EXAMPLE=ON \
      -DMORPH_BUILD_NET=ON -DMORPH_BUILD_FORMS_QML=ON -DMORPH_BUILD_OFFLINE_SQLITE=ON
# Without either frontend, to prove the app libraries and their tests do not need one (Tasks K16, L11, M7):
cmake -S . -B build/nofront -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON \
      -DMORPH_LADDER_RUNGS="kanban;ledger;lims" -DMORPH_BUILD_NET=ON
# Per task:
cmake --build build/all --target ladder_kanban_tests     # or ladder_ledger_tests / ladder_lims_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client]"
```

Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING, "Warnings
are errors").

---
# Group `kanban`

### Task K1: The domain library loses Qt — the attachment server moves to the server

The HTTP attachment side channel is a `QTcpServer`. It runs only in `ladder_kanban_server` and in the tests that
exercise it, so it moves to the server side by Part 8's convention: `src/server/app/*.cpp` with headers under
`src/server/include/` become `ladder_kanban_server_app`, which the server and `ladder_kanban_tests` link. The header
keeps its include spelling, `"kanban/http/attachment_server.hpp"`, so no includer changes. `ladder_kanban_lib` then
needs no Qt module of its own (kanban's `App` in `src/app/` is already Qt-free and stays); Part 8's `morph_add_rung`
drops its remaining `Qt6::Core` link once the rung has `app/` (Task K2).

**Files:**
- Move: `examples/kanban/include/kanban/http/attachment_server.hpp` →
  `examples/kanban/src/server/include/kanban/http/attachment_server.hpp`
- Move: `examples/kanban/src/http/attachment_server.cpp` → `examples/kanban/src/server/app/attachment_server.cpp`
- Modify: `examples/kanban/CMakeLists.txt` — replace the "Task 17" block (the one that adds
  `src/http/attachment_server.cpp` to `ladder_kanban_lib` and links `Qt6::Network` to it)
- Test: `examples/kanban/tests/test_attachment_server.cpp` (unchanged cases), plus the include check below

**Interfaces:**
- Consumes: `kanban::http::AttachmentServer` (unchanged); Part 8's `morph_add_rung` rule `src/server/app/*.cpp` +
  `src/server/include/` → `ladder_<rung>_server_app` (linked by `ladder_<rung>_server` and `ladder_<rung>_tests`).
- Produces: `ladder_kanban_server_app` carrying `AttachmentServer` and `Qt6::Network`; the header still included as
  `"kanban/http/attachment_server.hpp"` by the server, the tests and (Tasks K10, K11) the transfer tests.

- [ ] **Step 1: Write the failing check**

```bash
git grep -n -E '#include <Q' -- examples/kanban/include examples/kanban/src \
    ':!examples/kanban/src/server' ':!examples/kanban/src/headless'
test $? -eq 1 && echo "domain library is Qt-free"
```

- [ ] **Step 2: Run it to verify it fails**

Run the check above. Expected: it lists `examples/kanban/include/kanban/http/attachment_server.hpp:4:#include
<QHostAddress>` (and three more `<Q…>` lines) and `examples/kanban/src/http/attachment_server.cpp:6:#include
<QByteArray>`, and does not print "domain library is Qt-free".

- [ ] **Step 3: Implement**

```bash
mkdir -p examples/kanban/src/server/include/kanban/http examples/kanban/src/server/app
git mv examples/kanban/include/kanban/http/attachment_server.hpp \
       examples/kanban/src/server/include/kanban/http/attachment_server.hpp
git mv examples/kanban/src/http/attachment_server.cpp examples/kanban/src/server/app/attachment_server.cpp
```

In `examples/kanban/CMakeLists.txt`, replace the whole comment-and-`if` block that begins
`# Task 17: the attachment HTTP side channel` (through its `endif()`) with:

```cmake
# The attachment side channel (src/server/app/attachment_server.cpp) is a QTcpServer. It lives in
# ladder_kanban_server_app, which only the server and the tests link, so the domain library stays toolkit-free.
if(TARGET ladder_kanban_server_app)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Network)
    target_link_libraries(ladder_kanban_server_app PUBLIC Qt6::Network)
endif()
```

The block that links `Qt6::Network` onto `ladder_kanban_gui_lib` stays until Task K15 deletes `gui_lib/`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests ladder_kanban_server
./build/all/examples/kanban/ladder_kanban_tests "[kanban][attachments]"
git grep -n -E '#include <Q' -- examples/kanban/include examples/kanban/src \
    ':!examples/kanban/src/server' ':!examples/kanban/src/headless'; test $? -eq 1 && echo "domain library is Qt-free"
```

Expected: both targets build; every `[kanban][attachments]` case passes; the check prints "domain library is
Qt-free".

Mutation check: add `#include <QObject>` as the first include of `examples/kanban/src/models/board_model.cpp`.
Expected: the check lists that line and does not print "domain library is Qt-free". Remove it.

- [ ] **Step 5: Commit**

```bash
git add -A examples/kanban/include/kanban/http examples/kanban/src/http examples/kanban/src/server \
        examples/kanban/CMakeLists.txt
git commit -m "wip(kanban): the attachment server moves to the server; the domain library is Qt-free

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K2: The kanban app library — display strings

The app library's first file. What every controller is built from is shared, not kanban's: `morph::examples::Wiring`
(`"app/wiring.hpp"`), `morph::examples::mapCompletion` (`"app/completion_map.hpp"`, tested in `examples/common`) and
`morph::forms::FormModel::forAction<A>()` (Part 5) — so this task adds only the rung's own display strings, and
`morph_add_rung` builds `app/` into `ladder_kanban_app` and links it into `ladder_kanban_tests` with no CMake line of
this rung's.

**Files:**
- Create: `examples/kanban/app/support/board_format.hpp`, `examples/kanban/app/support/board_format.cpp`
- Test: `examples/kanban/tests/client/test_board_format.cpp`

**Interfaces:**
- Consumes: `kanban::roleToString`, `ruleMutationTypeToString` and the DTOs named below (`kanban/core/types.hpp`,
  `kanban/dto/*.hpp`); Parts 6 and 8's `morph_add_rung` (`app/` → `ladder_kanban_app`, include dir `app/`).
- Produces (later tasks use exactly these):
  - `kanban::client::columnHeader(ColumnView const&)`, `roleLabel(Role)`, `projectLine(MyProjectSummary const&)`,
    `ruleLine(RuleView const&, std::vector<ColumnView> const&)`, `activityMeta(ActivityEvent const&)`,
    `pendingSyncLine(int)`, `deadLetterLine(int)`, `attachmentLine(AttachmentView const&)` — all `-> std::string`.

- [ ] **Step 1: Write the failing tests**

Create `examples/kanban/tests/client/test_board_format.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "support/board_format.hpp"

using kanban::ColumnId;
using kanban::ColumnView;

TEST_CASE("kanban::client::columnHeader shows the count, and the WIP limit when there is one", "[kanban][client]") {
    CHECK(kanban::client::columnHeader(ColumnView{.id = ColumnId{1}, .name = "To Do", .wipLimit = 0, .taskCount = 2}) ==
          "To Do  (2)");
    CHECK(kanban::client::columnHeader(ColumnView{.id = ColumnId{1}, .name = "Doing", .wipLimit = 3, .taskCount = 2}) ==
          "Doing  (2/3)");
}

TEST_CASE("kanban::client::ruleLine names the trigger column, or its id when the column is unknown",
          "[kanban][client]") {
    std::vector<ColumnView> const columns{ColumnView{.id = ColumnId{7}, .name = "Done", .wipLimit = 0, .taskCount = 0}};
    kanban::RuleView rule{.id = kanban::RuleId{1},
                          .triggerColumnId = ColumnId{7},
                          .mutationType = kanban::RuleMutationType::AddTag,
                          .mutationValue = "shipped"};
    CHECK(kanban::client::ruleLine(rule, columns) == "when moved to \"Done\": AddTag \"shipped\"");
    rule.triggerColumnId = ColumnId{9};
    rule.mutationType = kanban::RuleMutationType::RemoveTag;
    CHECK(kanban::client::ruleLine(rule, columns) == "when moved to \"9\": RemoveTag \"shipped\"");
}

TEST_CASE("kanban::client sync lines are empty at zero and count otherwise", "[kanban][client]") {
    CHECK(kanban::client::pendingSyncLine(0).empty());
    CHECK(kanban::client::pendingSyncLine(2) == "2 changes pending sync");
    CHECK(kanban::client::deadLetterLine(0).empty());
    CHECK(kanban::client::deadLetterLine(1) == "1 changes could not be synced");
}

TEST_CASE("kanban::client::projectLine, activityMeta and attachmentLine join their parts", "[kanban][client]") {
    CHECK(kanban::client::projectLine(kanban::MyProjectSummary{
              .id = kanban::ProjectId{3}, .name = "Sprint", .myRole = kanban::Role::Manager}) == "Sprint  ·  Manager");
    CHECK(kanban::client::activityMeta(kanban::ActivityEvent{
              .actionType = "MoveTaskPosition", .principal = "alice", .timestampMs = 0, .summary = "moved"}) ==
          "alice · MoveTaskPosition");
    CHECK(kanban::client::attachmentLine(kanban::AttachmentView{.id = kanban::AttachmentId{1},
                                                                .taskId = kanban::TaskId{2},
                                                                .filename = "report.pdf",
                                                                .contentType = "application/pdf",
                                                                .sizeBytes = 34,
                                                                .storageKey = "k",
                                                                .uploadedBy = "alice",
                                                                .uploadedAtMs = 0}) ==
          "report.pdf  (34 bytes, alice)");
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'support/board_format.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/support/board_format.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

#include "kanban/core/types.hpp"
#include "kanban/dto/activity_dto.hpp"
#include "kanban/dto/attachment_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/dto/rule_dto.hpp"

namespace kanban::client {

/// @brief A column's header: its name and task count, plus the WIP limit when it has one.
/// @param column The column.
/// @return E.g. `"To Do  (2)"` or `"Doing  (2/3)"`.
[[nodiscard]] std::string columnHeader(ColumnView const& column);

/// @brief A role's display name.
/// @param role The role.
/// @return `"Viewer"`, `"Member"` or `"Manager"`.
[[nodiscard]] std::string roleLabel(Role role);

/// @brief A project row in the project list.
/// @param project The project and the caller's role on it.
/// @return E.g. `"Sprint  ·  Manager"`.
[[nodiscard]] std::string projectLine(MyProjectSummary const& project);

/// @brief A rule row: the trigger column by name (its id when the column is gone) and the mutation.
/// @param rule The rule.
/// @param columns The board's columns.
/// @return E.g. `when moved to "Done": AddTag "shipped"`.
[[nodiscard]] std::string ruleLine(RuleView const& rule, std::vector<ColumnView> const& columns);

/// @brief The second line of an activity entry: who did what.
/// @param event The entry.
/// @return E.g. `"alice · MoveTaskPosition"`.
[[nodiscard]] std::string activityMeta(ActivityEvent const& event);

/// @brief The pending-sync banner.
/// @param queueDepth Moves waiting in the offline queue.
/// @return Empty at zero, else `"<n> changes pending sync"`.
[[nodiscard]] std::string pendingSyncLine(int queueDepth);

/// @brief The dead-letter banner.
/// @param deadLettered Moves the server kept refusing until their retry budget ran out.
/// @return Empty at zero, else `"<n> changes could not be synced"`.
[[nodiscard]] std::string deadLetterLine(int deadLettered);

/// @brief An attachment row.
/// @param attachment The attachment's metadata.
/// @return E.g. `"report.pdf  (34 bytes, alice)"`.
[[nodiscard]] std::string attachmentLine(AttachmentView const& attachment);

}  // namespace kanban::client
```

Create `examples/kanban/app/support/board_format.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "support/board_format.hpp"

#include <string>

namespace kanban::client {

std::string columnHeader(ColumnView const& column) {
    std::string header = column.name + "  (" + std::to_string(column.taskCount);
    if (column.wipLimit > 0) {
        header += "/" + std::to_string(column.wipLimit);
    }
    return header + ")";
}

std::string roleLabel(Role role) { return std::string{roleToString(role)}; }

std::string projectLine(MyProjectSummary const& project) { return project.name + "  ·  " + roleLabel(project.myRole); }

std::string ruleLine(RuleView const& rule, std::vector<ColumnView> const& columns) {
    std::string column = rule.triggerColumnId.hasValue() ? std::to_string(*rule.triggerColumnId) : std::string{"?"};
    for (ColumnView const& candidate : columns) {
        if (candidate.id == rule.triggerColumnId) {
            column = candidate.name;
            break;
        }
    }
    return "when moved to \"" + column + "\": " + std::string{ruleMutationTypeToString(rule.mutationType)} + " \"" +
           rule.mutationValue + "\"";
}

std::string activityMeta(ActivityEvent const& event) { return event.principal + " · " + event.actionType; }

std::string pendingSyncLine(int queueDepth) {
    return queueDepth > 0 ? std::to_string(queueDepth) + " changes pending sync" : std::string{};
}

std::string deadLetterLine(int deadLettered) {
    return deadLettered > 0 ? std::to_string(deadLettered) + " changes could not be synced" : std::string{};
}

std::string attachmentLine(AttachmentView const& attachment) {
    return attachment.filename + "  (" + std::to_string(attachment.sizeBytes) + " bytes, " + attachment.uploadedBy +
           ")";
}

}  // namespace kanban::client
```

Re-run the configure (`cmake build/all`) so `morph_add_rung`'s globs see the new `app/` and `tests/client/` files:
the first `app/*.cpp` is what creates `ladder_kanban_app`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client]"
```

Expected: PASS, 4 test cases.

Mutation check: in `columnHeader`, drop the `if (column.wipLimit > 0)` block. Expected FAIL in "columnHeader shows
the count, and the WIP limit when there is one" (`"Doing  (2)"`). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app examples/kanban/tests/client
git commit -m "wip(kanban): app library foundations: display strings

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K3: `SessionController` — sign-in, with the token kept off the screen

Re-expresses `ProjectAdminPresenter::login`/`onLoginSucceeded` and `ProjectAdminBridge::submitIfValid("Login")`
(`gui_lib/project_admin_presenter.cpp`): the schema-driven `Login` form submits through `AuthModel`; a success
installs `{principal, token}` as the bridge's default session; the reply the form shows is the `LoginResult` with the
token cleared.

**Files:**
- Create: `examples/kanban/app/controllers/session_controller.hpp`, `examples/kanban/app/controllers/session_controller.cpp`
- Create: `examples/kanban/tests/client/kanban_client_support.hpp`
- Test: `examples/kanban/tests/client/test_session_controller.cpp`

**Interfaces:**
- Consumes: `morph::examples::Wiring`, `mapCompletion` (Part 6); `forms::FormModel::forAction<A>()`,
  `forms::FormSession(Runtime&, FormModel, Submitter, ChoiceFetcher, FormSessionOptions)`, `assign`, `submit`,
  `ready`, `pending`, `lastReply`, `lastError` (Part 5);
  `BridgeHandler<AuthModel>::executeJson(std::string_view, std::string_view)` (`morph/core/bridge.hpp:2312`);
  `Bridge::setDefaultSession`, `defaultSession` (`bridge.hpp:988`, `:1037`); `TokenVerifier::verify(token, nowMs)`
  (`morph/session/session_auth.hpp:460`); `kanban::auth::setTokenIssuer`.
- Produces:
  - `kanban::client::SessionController(examples::Wiring)`: `loginForm() -> forms::FormSession&`, `signedIn() -> bool`,
    `principal() -> std::string const&`, `headerText() -> std::string` (all reads tracked).
  - Test support (`kanban::testing`): `kSecret`, `signedContext(principal)`, `IssuerScope`, `LocalClient{pool, owner,
    bridge, runtime, scheduler; settle(pred, budget); wiring()}`, `await(client, completion)`,
    `seedProject(client, name) -> ProjectId`, `fill(form, json)`, `submitForm(client, form, json)`,
    `submitFormExpectingRefusal(client, form, json) -> std::string`, `signIn(client, session, principal)`.

- [ ] **Step 1: Write the failing test**

Create `examples/kanban/tests/client/kanban_client_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "app/wiring.hpp"
#include "controllers/session_controller.hpp"
#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/wait.hpp"

namespace kanban::testing {

using namespace std::chrono_literals;

/// The signing secret every kanban client test shares.
inline constexpr std::string_view kSecret = "kanban-client-test-secret-32-bytes!!";

/// A session for @p principal whose token is signed with `kSecret`.
[[nodiscard]] inline morph::session::Context signedContext(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    morph::session::Context context;
    context.principal = principal;
    context.token = issuer.issue(morph::session::SessionToken{
        .principal = principal, .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return context;
}

/// Installs the process-wide issuer `Login` mints tokens from, for one test, and removes it after.
class IssuerScope {
public:
    IssuerScope() {
        kanban::auth::setTokenIssuer(
            std::make_shared<morph::session::TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256));
    }
    ~IssuerScope() { kanban::auth::setTokenIssuer(nullptr); }
    IssuerScope(IssuerScope const&) = delete;
    IssuerScope& operator=(IssuerScope const&) = delete;
    IssuerScope(IssuerScope&&) = delete;
    IssuerScope& operator=(IssuerScope&&) = delete;
};

/// One client over a local backend: the owner every completion and every flush runs on, a manual clock for
/// timed refreshes, and the bridge. Declare it before the controllers under test, so they are destroyed first.
class LocalClient {
public:
    LocalClient() = default;
    explicit LocalClient(morph::session::Context session) { bridge.setDefaultSession(std::move(session)); }
    ~LocalClient() = default;
    LocalClient(LocalClient const&) = delete;
    LocalClient& operator=(LocalClient const&) = delete;
    LocalClient(LocalClient&&) = delete;
    LocalClient& operator=(LocalClient&&) = delete;

    /// Pumps the owner until @p done holds or the budget runs out.
    template <typename Pred>
    [[nodiscard]] bool settle(Pred done, std::chrono::milliseconds budget = 5000ms) {
        return morph::examples::testing::pumpUntil(owner, std::move(done), budget);
    }

    /// What a controller under test is built from.
    [[nodiscard]] morph::examples::Wiring wiring() {
        return morph::examples::Wiring{
            .runtime = runtime, .scheduler = scheduler, .bridge = bridge, .callbacks = owner};
    }

    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::reactive::Runtime runtime{owner};
    morph::reactive::testing::ManualScheduler scheduler;
};

/// Waits for @p completion on @p client's owner and returns its value, or rethrows its failure.
template <typename Client, typename T>
[[nodiscard]] T await(Client& client, morph::async::Completion<T> completion) {
    struct State {
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto state = std::make_shared<State>();
    completion.thenDetached([state](T const& value) { state->value = value; })
        .onErrorDetached([state](std::exception_ptr error) { state->error = std::move(error); });
    if (!client.settle([&state] { return state->value.has_value() || state->error != nullptr; })) {
        throw std::runtime_error{"await: the completion did not settle"};
    }
    if (state->error != nullptr) {
        std::rethrow_exception(state->error);
    }
    return std::move(*state->value);
}

/// Creates a project as the client's principal, who becomes its Manager.
[[nodiscard]] inline kanban::ProjectId seedProject(LocalClient& client, std::string name) {
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> admin{client.bridge, &client.owner};
    return await(client, admin.execute(kanban::CreateProject{.name = std::move(name)})).id;
}

/// Sets each member @p bodyJson names, as typing into those fields would, and leaves every other field alone — the
/// context its controller filled in (a project, a task) stays. `prefill` would blank that context.
inline void fill(morph::forms::FormSession& form, std::string_view bodyJson) {
    glz::generic_u64 members;
    REQUIRE_FALSE(glz::read_json(members, std::string{bodyJson}));
    REQUIRE(members.is_object());
    for (auto const& [name, value] : members.get_object()) {
        form.assign(name, glz::write_json(value).value_or(std::string{"null"}));
    }
}

/// Fills @p form, submits it, waits for the outcome and requires success.
template <typename Client>
void submitForm(Client& client, morph::forms::FormSession& form, std::string_view bodyJson) {
    fill(form, bodyJson);
    REQUIRE(form.ready());
    form.submit();
    REQUIRE(client.settle([&form] { return !form.pending(); }));
    INFO("the form reported: " << morph::reactive::errorMessage(form.lastError()));
    REQUIRE(form.lastError() == nullptr);
}

/// Fills @p form, submits it, waits for the outcome, requires a refusal and returns its message.
template <typename Client>
[[nodiscard]] std::string submitFormExpectingRefusal(Client& client, morph::forms::FormSession& form,
                                                     std::string_view bodyJson) {
    fill(form, bodyJson);
    REQUIRE(form.ready());
    form.submit();
    REQUIRE(client.settle([&form] { return !form.pending(); }));
    REQUIRE(form.lastError() != nullptr);
    return morph::reactive::errorMessage(form.lastError());
}

/// Signs @p principal in through @p session's form. Needs an `IssuerScope`.
template <typename Client>
void signIn(Client& client, kanban::client::SessionController& session, std::string const& principal) {
    submitForm(client, session.loginForm(), R"({"username":")" + principal + R"("})");
    REQUIRE(session.signedIn());
}

}  // namespace kanban::testing
```

Create `examples/kanban/tests/client/test_session_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/session/session_auth.hpp>
#include <string>

#include "controllers/session_controller.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"

using kanban::testing::IssuerScope;
using kanban::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

TEST_CASE("kanban::client::SessionController: signing in installs the minted token as the default session",
          "[kanban][client]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    kanban::client::SessionController session{client.wiring()};
    CHECK_FALSE(session.signedIn());
    CHECK(session.headerText() == "not signed in");

    kanban::testing::signIn(client, session, "alice");

    CHECK(session.principal() == "alice");
    CHECK(session.headerText() == "signed in as alice");
    CHECK(client.bridge.defaultSession().principal == "alice");
    morph::session::TokenVerifier const verifier{std::string{kanban::testing::kSecret}, morph::session::hmacSha256};
    auto const nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    auto const verified = verifier.verify(client.bridge.defaultSession().token, nowMs);
    REQUIRE(verified.has_value());
    CHECK(verified->principal == "alice");
}

TEST_CASE("kanban::client::SessionController: the sign-in reply on screen carries no token", "[kanban][client]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    kanban::client::SessionController session{client.wiring()};
    kanban::testing::signIn(client, session, "alice");

    std::string const token = client.bridge.defaultSession().token;
    REQUIRE_FALSE(token.empty());
    auto const& reply = session.loginForm().lastReply();
    REQUIRE(reply.has_value());
    CHECK(reply->find(token) == std::string::npos);
    CHECK(reply->find(R"("principal":"alice")") != std::string::npos);
}

TEST_CASE("kanban::client::SessionController: a refused sign-in leaves the bridge unauthenticated",
          "[kanban][client]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    kanban::client::SessionController session{client.wiring()};

    std::string const message =
        kanban::testing::submitFormExpectingRefusal(client, session.loginForm(), R"({"username":"system:root"})");

    CHECK_FALSE(message.empty());
    CHECK_FALSE(session.signedIn());
    CHECK(client.bridge.defaultSession().principal.empty());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'controllers/session_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/controllers/session_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/signal.hpp>
#include <string>
#include <string_view>

#include "app/wiring.hpp"
#include "kanban/models/project_admin_model.hpp"

namespace kanban::client {

/// @brief The sign-in screen: a schema-driven `Login` form whose success installs the minted token as the bridge's
///        default session.
///
/// The reply the form shows is the server's `LoginResult` with the token cleared: the token is a credential, and
/// the screen has no use for it.
class SessionController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    explicit SessionController(morph::examples::Wiring wiring);

    /// @brief The `Login` form.
    /// @return The form session; it lives as long as this controller.
    [[nodiscard]] morph::forms::FormSession& loginForm() noexcept { return _login; }

    /// @brief Whether a sign-in succeeded. Tracked.
    /// @return True once a session is installed.
    [[nodiscard]] bool signedIn() const { return !_principal.get().empty(); }

    /// @brief The signed-in principal, as the server echoed it. Tracked.
    /// @return The principal, empty before sign-in.
    [[nodiscard]] std::string const& principal() const { return _principal.get(); }

    /// @brief The header line naming who is signed in. Tracked.
    /// @return `"signed in as <principal>"` or `"not signed in"`.
    [[nodiscard]] std::string headerText() const;

private:
    [[nodiscard]] morph::async::Completion<std::string> signIn(std::string_view actionType, std::string body);

    morph::examples::Wiring _wiring;
    morph::bridge::BridgeHandler<AuthModel> _auth;
    morph::reactive::Signal<std::string> _principal;
    morph::forms::FormSession _login;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace kanban::client
```

Create `examples/kanban/app/controllers/session_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/session_controller.hpp"

#include <exception>
#include <glaze/glaze.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <utility>

#include "app/completion_map.hpp"

namespace kanban::client {

SessionController::SessionController(morph::examples::Wiring wiring)
    : _wiring{wiring},
      _auth{wiring.bridge, &wiring.callbacks},
      _principal{wiring.runtime, std::string{}},
      _login{wiring.runtime, morph::forms::FormModel::forAction<Login>(),
             [this](std::string_view actionType, std::string body) { return signIn(actionType, std::move(body)); },
             {}} {}

std::string SessionController::headerText() const {
    std::string const& name = _principal.get();
    return name.empty() ? std::string{"not signed in"} : "signed in as " + name;
}

morph::async::Completion<std::string> SessionController::signIn(std::string_view actionType, std::string body) {
    return morph::examples::mapCompletion<std::string>(
        _wiring.callbacks, _lifetime.token(), _auth.executeJson(actionType, body),
        [this](std::string const& reply) {
            LoginResult result;
            if (glz::read_json(result, reply)) {
                throw std::runtime_error{"sign-in succeeded but its reply could not be read"};
            }
            morph::session::Context session;
            session.principal = result.principal;
            session.token = result.token.hasValue() ? *result.token : std::string{};
            _wiring.bridge.setDefaultSession(std::move(session));
            _principal.set(result.principal);
            result.token = AuthToken{};
            return glz::write_json(result).value_or(std::string{"{}"});
        },
        [](std::exception_ptr const&) {});
}

}  // namespace kanban::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client]"
```

Expected: PASS (7 cases so far).

Mutation check: delete `result.token = AuthToken{};`. Expected FAIL in "the sign-in reply on screen carries no
token" (`reply->find(token)` finds it). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/controllers/session_controller.hpp examples/kanban/app/controllers/session_controller.cpp \
        examples/kanban/tests/client/kanban_client_support.hpp examples/kanban/tests/client/test_session_controller.cpp
git commit -m "wip(kanban): SessionController: sign-in that installs the token and keeps it off the screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K4: `ProjectsController` — projects, the create form, and members with the role picker

Re-expresses `ProjectAdminPresenter`/`ProjectAdminBridge` (`refreshProjects`, `createProject`, `listRoles`,
`setMemberRole`, `removeMember`, the `CreateProject` and `SetMemberRole` forms) and `ProjectListView.qml`'s and
`MembersView.qml`'s conditionals. The project list is a `Query<GetMyProjects>` keyed on being signed in; the members
panel is a `Query<GetProjectRoles>` keyed on the project whose members are shown; the per-row role picker and
Remove are `Mutation`s invalidating it.

**Files:**
- Create: `examples/kanban/app/controllers/projects_controller.hpp`, `examples/kanban/app/controllers/projects_controller.cpp`
- Test: `examples/kanban/tests/client/test_projects_controller.cpp`

**Interfaces:**
- Consumes: `SessionController::signedIn()` (Task K3); `reactive::Query`, `Mutation`, `MutationOptions` (Part 1);
  `projectLine` (Task K2); `morph::examples::mapCompletion` (Part 6); `forms::FormModel::forAction<A>()`,
  `FormSession::assign`, `reset` (Part 5).
- Produces: `kanban::client::ProjectRow{id, name, role, label}`, `MemberRow{principal, role}`;
  `ProjectsController(examples::Wiring, SessionController const&)`: `refresh()`, `projects()`, `loading()`,
  `createProjectForm()`, `showMembers(ProjectId, std::string name)`, `hideMembers()`, `membersShown()`,
  `membersTitle()`, `members()`, `setRole(std::string principal, Role)`, `removeMember(std::string principal)`,
  `addMemberForm()`, `errorText()`, static `roleNames() -> std::vector<std::string>`.

- [ ] **Step 1: Write the failing test**

Create `examples/kanban/tests/client/test_projects_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "controllers/projects_controller.hpp"
#include "controllers/session_controller.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"

using kanban::client::MemberRow;
using kanban::client::ProjectsController;
using kanban::client::SessionController;
using kanban::testing::IssuerScope;
using kanban::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

namespace {

[[nodiscard]] bool hasMember(ProjectsController const& projects, std::string const& principal, kanban::Role role) {
    auto const members = projects.members();
    return std::ranges::find(members, MemberRow{.principal = principal, .role = role}) != members.end();
}

}  // namespace

TEST_CASE("kanban::client::ProjectsController: nothing is listed before sign-in, then the caller's projects are",
          "[kanban][client]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    SessionController session{client.wiring()};
    ProjectsController projects{client.wiring(), session};
    CHECK_FALSE(projects.loading());
    CHECK(projects.projects().empty());

    kanban::testing::signIn(client, session, "alice");
    REQUIRE(client.settle([&projects] { return !projects.loading(); }));
    CHECK(projects.projects().empty());

    kanban::testing::submitForm(client, projects.createProjectForm(), R"({"name":"Sprint Board"})");
    REQUIRE(client.settle([&projects] { return projects.projects().size() == 1; }));
    auto const row = projects.projects().front();
    CHECK(row.name == "Sprint Board");
    CHECK(row.role == kanban::Role::Manager);
    CHECK(row.label == "Sprint Board  ·  Manager");
    CHECK(row.id > 0);
}

TEST_CASE("kanban::client::ProjectsController: members round-trip through the add form, the role picker and Remove",
          "[kanban][client]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    SessionController session{client.wiring()};
    ProjectsController projects{client.wiring(), session};
    kanban::testing::signIn(client, session, "alice");
    kanban::testing::submitForm(client, projects.createProjectForm(), R"({"name":"Sprint Board"})");
    REQUIRE(client.settle([&projects] { return projects.projects().size() == 1; }));
    auto const project = projects.projects().front();

    projects.showMembers(kanban::ProjectId{project.id}, project.name);
    CHECK(projects.membersShown());
    CHECK(projects.membersTitle() == "Members of Sprint Board");
    REQUIRE(client.settle([&] { return hasMember(projects, "alice", kanban::Role::Manager); }));

    kanban::testing::submitForm(client, projects.addMemberForm(), R"({"principal":"bob","role":"Member"})");
    REQUIRE(client.settle([&] { return hasMember(projects, "bob", kanban::Role::Member); }));

    projects.setRole("bob", kanban::Role::Manager);
    REQUIRE(client.settle([&] { return hasMember(projects, "bob", kanban::Role::Manager); }));

    projects.removeMember("bob");
    REQUIRE(client.settle([&projects] { return projects.members().size() == 1; }));
    CHECK(hasMember(projects, "alice", kanban::Role::Manager));
    CHECK(projects.errorText().empty());

    projects.hideMembers();
    CHECK_FALSE(projects.membersShown());
    CHECK(projects.members().empty());
}

TEST_CASE("kanban::client::ProjectsController: a refused member change surfaces in errorText", "[kanban][client]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    SessionController session{client.wiring()};
    ProjectsController projects{client.wiring(), session};
    kanban::testing::signIn(client, session, "alice");
    kanban::testing::submitForm(client, projects.createProjectForm(), R"({"name":"Sprint Board"})");
    REQUIRE(client.settle([&projects] { return projects.projects().size() == 1; }));
    auto const project = projects.projects().front();
    projects.showMembers(kanban::ProjectId{project.id}, project.name);

    projects.removeMember("");

    REQUIRE(client.settle([&projects] { return !projects.errorText().empty(); }));
}

TEST_CASE("kanban::client::ProjectsController::roleNames lists every role in rank order", "[kanban][client]") {
    CHECK(ProjectsController::roleNames() == std::vector<std::string>{"Viewer", "Member", "Manager"});
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'controllers/projects_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/controllers/projects_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/wiring.hpp"
#include "controllers/session_controller.hpp"
#include "kanban/models/project_admin_model.hpp"

namespace kanban::client {

/// @brief One project in the project list.
struct ProjectRow {
    /// @brief The project's id.
    std::int64_t id = 0;
    /// @brief Its name.
    std::string name;
    /// @brief The caller's role on it.
    Role role = Role::Viewer;
    /// @brief The row's display line.
    std::string label;
    /// @brief Rows compare by value, so an unchanged row keeps its widgets.
    bool operator==(ProjectRow const&) const = default;
};

/// @brief One member in the members panel.
struct MemberRow {
    /// @brief The member's principal.
    std::string principal;
    /// @brief Their role.
    Role role = Role::Viewer;
    /// @brief Rows compare by value.
    bool operator==(MemberRow const&) const = default;
};

/// @brief The project list screen and its members panel.
class ProjectsController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    /// @param session Gates the project list on being signed in. Borrowed: it must outlive this controller.
    ProjectsController(morph::examples::Wiring wiring, SessionController const& session);

    /// @brief Fetches the project list again.
    void refresh();

    /// @brief The caller's projects. Tracked.
    /// @return One row per project, in the server's order; empty before sign-in.
    [[nodiscard]] std::vector<ProjectRow> projects() const;

    /// @brief Whether the project list is being fetched. Tracked.
    /// @return True while a fetch is in flight.
    [[nodiscard]] bool loading() const { return _projects.pending(); }

    /// @brief The `CreateProject` form.
    /// @return The form session.
    [[nodiscard]] morph::forms::FormSession& createProjectForm() noexcept { return _createProject; }

    /// @brief Shows the members of @p project.
    /// @param project The project.
    /// @param name Its name, for the panel's title.
    void showMembers(ProjectId project, std::string name);

    /// @brief Hides the members panel.
    void hideMembers();

    /// @brief Whether the members panel is shown. Tracked.
    /// @return True after `showMembers`, until `hideMembers`.
    [[nodiscard]] bool membersShown() const { return _membersOf.get().has_value(); }

    /// @brief The members panel's title. Tracked.
    /// @return `"Members of <name>"`, or `"Members"` when no project is chosen.
    [[nodiscard]] std::string membersTitle() const;

    /// @brief The shown project's members. Tracked.
    /// @return One row per member; empty while hidden.
    [[nodiscard]] std::vector<MemberRow> members() const;

    /// @brief Changes a member's role on the shown project; the members list refreshes when it lands.
    /// @param principal The member.
    /// @param role The new role.
    void setRole(std::string principal, Role role);

    /// @brief Removes a member from the shown project; the members list refreshes when it lands.
    /// @param principal The member.
    void removeMember(std::string principal);

    /// @brief The `SetMemberRole` form that adds a member; its `projectId` is the shown project.
    /// @return The form session.
    [[nodiscard]] morph::forms::FormSession& addMemberForm() noexcept { return _addMember; }

    /// @brief The first failure among the list, the members and the member changes. Tracked.
    /// @return The message, or empty.
    [[nodiscard]] std::string errorText() const;

    /// @brief The roles the role picker offers, lowest first.
    /// @return `{"Viewer", "Member", "Manager"}`.
    [[nodiscard]] static std::vector<std::string> roleNames();

private:
    struct MembersOf {
        ProjectId project;
        std::string name;
        bool operator==(MembersOf const&) const = default;
    };

    [[nodiscard]] morph::async::Completion<std::string> submit(std::string_view actionType, std::string body,
                                                               std::function<void()> after);
    void assignMemberContext();

    morph::examples::Wiring _wiring;
    morph::bridge::BridgeHandler<ProjectAdminModel> _admin;
    morph::reactive::Signal<std::optional<MembersOf>> _membersOf;
    morph::reactive::Query<GetMyProjects> _projects;
    morph::reactive::Query<GetProjectRoles> _roles;
    morph::reactive::Mutation<SetMemberRole> _setRole;
    morph::reactive::Mutation<RemoveMember> _remove;
    morph::forms::FormSession _createProject;
    morph::forms::FormSession _addMember;
    // Last member, so it is destroyed first: a form reply still in flight finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace kanban::client
```

Create `examples/kanban/app/controllers/projects_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/projects_controller.hpp"

#include <exception>
#include <morph/forms/engine/field_model.hpp>
#include <utility>

#include "app/completion_map.hpp"
#include "support/board_format.hpp"

namespace kanban::client {

ProjectsController::ProjectsController(morph::examples::Wiring wiring, SessionController const& session)
    : _wiring{wiring},
      _admin{wiring.bridge, &wiring.callbacks},
      _membersOf{wiring.runtime, std::nullopt},
      _projects{wiring.runtime, _admin,
                [&session]() -> std::optional<GetMyProjects> {
                    if (!session.signedIn()) {
                        return std::nullopt;
                    }
                    return GetMyProjects{};
                }},
      _roles{wiring.runtime, _admin,
             [this]() -> std::optional<GetProjectRoles> {
                 auto const& target = _membersOf.get();
                 if (!target) {
                     return std::nullopt;
                 }
                 return GetProjectRoles{.projectId = target->project};
             }},
      _setRole{wiring.runtime, _admin, morph::reactive::MutationOptions{.invalidates = {&_roles}}},
      _remove{wiring.runtime, _admin, morph::reactive::MutationOptions{.invalidates = {&_roles}}},
      _createProject{wiring.runtime, morph::forms::FormModel::forAction<CreateProject>(),
                     [this](std::string_view actionType, std::string body) {
                         return submit(actionType, std::move(body), [this] {
                             _createProject.reset();
                             _projects.refetch();
                         });
                     },
                     {}},
      _addMember{wiring.runtime, morph::forms::FormModel::forAction<SetMemberRole>(),
                 [this](std::string_view actionType, std::string body) {
                     return submit(actionType, std::move(body), [this] {
                         _addMember.reset();
                         assignMemberContext();
                         _roles.refetch();
                     });
                 },
                 {}} {}

void ProjectsController::refresh() { _projects.refetch(); }

std::vector<ProjectRow> ProjectsController::projects() const {
    std::vector<ProjectRow> rows;
    auto const& listed = _projects.value();
    if (!listed) {
        return rows;
    }
    for (MyProjectSummary const& project : listed->projects) {
        if (!project.id.hasValue()) {
            continue;
        }
        rows.push_back(ProjectRow{
            .id = *project.id, .name = project.name, .role = project.myRole, .label = projectLine(project)});
    }
    return rows;
}

void ProjectsController::showMembers(ProjectId project, std::string name) {
    _membersOf.set(MembersOf{.project = project, .name = std::move(name)});
    _addMember.reset();
    assignMemberContext();
}

void ProjectsController::hideMembers() { _membersOf.set(std::nullopt); }

std::string ProjectsController::membersTitle() const {
    auto const& target = _membersOf.get();
    return target ? "Members of " + target->name : std::string{"Members"};
}

std::vector<MemberRow> ProjectsController::members() const {
    std::vector<MemberRow> rows;
    auto const& listed = _roles.value();
    if (!listed) {
        return rows;
    }
    for (MemberRole const& member : listed->roles) {
        rows.push_back(MemberRow{.principal = member.principal, .role = member.role});
    }
    return rows;
}

void ProjectsController::setRole(std::string principal, Role role) {
    auto const& target = _membersOf.peek();
    if (!target) {
        return;
    }
    _setRole.run(SetMemberRole{.projectId = target->project, .principal = std::move(principal), .role = role});
}

void ProjectsController::removeMember(std::string principal) {
    auto const& target = _membersOf.peek();
    if (!target) {
        return;
    }
    _remove.run(RemoveMember{.projectId = target->project, .principal = std::move(principal)});
}

std::string ProjectsController::errorText() const {
    for (std::exception_ptr const& error : {_projects.error(), _roles.error(), _setRole.error(), _remove.error()}) {
        if (error != nullptr) {
            return morph::reactive::errorMessage(error);
        }
    }
    return {};
}

std::vector<std::string> ProjectsController::roleNames() {
    return {std::string{roleToString(Role::Viewer)}, std::string{roleToString(Role::Member)},
            std::string{roleToString(Role::Manager)}};
}

morph::async::Completion<std::string> ProjectsController::submit(std::string_view actionType, std::string body,
                                                                 std::function<void()> after) {
    return morph::examples::mapCompletion<std::string>(
        _wiring.callbacks, _lifetime.token(), _admin.executeJson(actionType, body),
        [after = std::move(after)](std::string const& reply) {
            after();
            return reply;
        },
        [](std::exception_ptr const&) {});
}

void ProjectsController::assignMemberContext() {
    auto const& target = _membersOf.peek();
    if (target && target->project.hasValue()) {
        _addMember.assign("projectId", std::to_string(*target->project));
    }
}

}  // namespace kanban::client
```

`hideMembers` makes the roles query idle, which clears its value — so `members()` is empty while hidden, with no
conditional of its own.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client]"
```

Expected: PASS.

Mutation check: remove `_roles.refetch();` from the add-member form's success hook. Expected FAIL in "members
round-trip…" (bob never appears within the budget). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/controllers/projects_controller.hpp examples/kanban/app/controllers/projects_controller.cpp \
        examples/kanban/tests/client/test_projects_controller.cpp
git commit -m "wip(kanban): ProjectsController: project list, create form, members and the role picker

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K5: `OfflineMoves` — the durable offline queue, Qt-free

Re-expresses `BoardBridge::enableOfflineQueue`/`replayMoveTaskPosition` (`gui_lib/board_qml_bridge.cpp`) without a
nested `QEventLoop` and without running the reconnect sequence on the GUI thread. The queue and its drain run on a
strand over a private two-thread pool, and the reconnect sequence (with its retry sleeps) on the coordinator's own
strand over the same pool, as `ReconnectCoordinator`'s contract asks ("a worker pool",
`morph/offline/reconnect_coordinator.hpp:144`). A replay posts the move to the owner, where the bridge handler
belongs (`BridgeHandler`: "used and destroyed there", `bridge.hpp`), and waits for its outcome on the drain's
thread in short slices, so teardown never waits longer than one slice. The whole class is compiled only under
`MORPH_BUILD_OFFLINE_SQLITE`, as `BoardBridge`'s offline members were.

**Files:**
- Create: `examples/kanban/app/controllers/offline_config.hpp`
- Create: `examples/kanban/app/controllers/offline_moves.hpp`, `examples/kanban/app/controllers/offline_moves.cpp`
- Modify: `examples/kanban/CMakeLists.txt` — inside the existing `if(MORPH_BUILD_OFFLINE_SQLITE)` block, before its
  `if(TARGET ladder_kanban_tests)`, add the `ladder_kanban_app` lines below
- Modify: `examples/kanban/tests/client/kanban_client_support.hpp` — add `<filesystem>`, `<system_error>` to the
  includes and `ScopedQueueFile` (below) before the closing `}  // namespace kanban::testing`
- Test: `examples/kanban/tests/client/test_offline_moves.cpp`

**Interfaces:**
- Consumes: `morph::offline::SqliteOfflineQueue(IExecutor&, std::filesystem::path)`, `IOfflineQueue::enqueue(IExecutor&,
  std::string, std::string)` and `size(IExecutor&) const` (`morph/offline/offline_queue.hpp:325`, `:346`),
  `SyncWorker(IExecutor&, IOfflineQueue&, DetailedReplayFunction, DeadLetterSink)`, `SyncWorker::run(IExecutor&)`,
  `stop()` (`sync_worker.hpp:216`, `:240`, `:257`), `ReplayOutcome` (`sync_worker.hpp:33`),
  `ReconnectCoordinator(Deps, IExecutor&)`, `onOnline(IExecutor&)`, `onOffline()`, `strand()`
  (`reconnect_coordinator.hpp:152`, `:179`, `:195`, `:207`), `NetworkMonitor(ProbeFunction, Callback, Callback,
  Config)` (`network_monitor.hpp:97`), `morph::exec::OwnerStrand` (`morph/core/owner_strand.hpp:39`),
  `CallbackToken::guard` (`callback_scope.hpp:126`).
- Produces:
  - `kanban::client::OfflineConfig{queuePath, probe, probeInterval, failureThreshold, onlineThreshold}` (always).
  - `kanban::client::OfflineMoves` (only under `MORPH_BUILD_OFFLINE_SQLITE`): `OfflineMoves(Runtime&, IExecutor&
    owner, OfflineConfig const&, Replay, std::function<void()> onReplayed)`, `online()`, `enqueue(MoveTaskPosition
    const&)`, `queueDepth()`, `deadLettered()`, `drainsCompleted()`.
  - Test support: `kanban::testing::ScopedQueueFile(std::string name)` with `path()`.

- [ ] **Step 1: Write the failing test**

Add to `kanban_client_support.hpp` (with `#include <filesystem>` and `#include <system_error>`):

```cpp
/// A queue file under the temp directory, removed with its WAL side files before and after the test.
class ScopedQueueFile {
public:
    explicit ScopedQueueFile(std::string const& name)
        : _path{std::filesystem::temp_directory_path() / ("kanban_client_" + name + ".db")} {
        remove();
    }
    ~ScopedQueueFile() { remove(); }
    ScopedQueueFile(ScopedQueueFile const&) = delete;
    ScopedQueueFile& operator=(ScopedQueueFile const&) = delete;
    ScopedQueueFile(ScopedQueueFile&&) = delete;
    ScopedQueueFile& operator=(ScopedQueueFile&&) = delete;

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

private:
    void remove() const {
        std::error_code ignored;
        std::filesystem::remove(_path, ignored);
        std::filesystem::remove(_path.string() + "-wal", ignored);
        std::filesystem::remove(_path.string() + "-shm", ignored);
    }

    std::filesystem::path _path;
};
```

Create `examples/kanban/tests/client/test_offline_moves.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#ifdef MORPH_BUILD_OFFLINE_SQLITE

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/runtime.hpp>
#include <string>
#include <utility>
#include <vector>

#include "controllers/offline_moves.hpp"
#include "kanban/core/errors.hpp"
#include "kanban_client_support.hpp"

namespace {

using namespace std::chrono_literals;
using kanban::client::OfflineConfig;
using kanban::client::OfflineMoves;
using morph::async::Completion;
using morph::examples::testing::pumpUntil;

// An OfflineMoves over a fake replay the test controls, with a probe the test flips.
class Harness {
public:
    enum class Answer : std::uint8_t { Accept, Refuse, Hold };

    explicit Harness(std::filesystem::path const& queuePath)
        : moves{std::make_unique<OfflineMoves>(
              runtime, owner,
              OfflineConfig{.queuePath = queuePath,
                            .probe = [this] { return reachable.load(); },
                            .probeInterval = 20ms,
                            .failureThreshold = 1,
                            .onlineThreshold = 1},
              [this](kanban::MoveTaskPosition action) { return replay(std::move(action)); },
              [this] { ++drainsThatLanded; })} {}

    [[nodiscard]] bool settle(auto done, std::chrono::milliseconds budget = 5000ms) {
        return pumpUntil(owner, std::move(done), budget);
    }

    void goOffline() {
        reachable.store(false);
        REQUIRE(settle([this] { return !moves->online(); }));
    }

    void goOnline() { reachable.store(true); }

    morph::exec::MainThreadExecutor owner;
    morph::reactive::Runtime runtime{owner};
    std::atomic<bool> reachable{true};
    Answer answer = Answer::Accept;
    std::vector<std::string> replayed;
    std::vector<Completion<kanban::GetBoardResult>::Promise> held;
    int drainsThatLanded = 0;
    std::unique_ptr<OfflineMoves> moves;

private:
    Completion<kanban::GetBoardResult> replay(kanban::MoveTaskPosition action) {
        replayed.push_back(action.opId);
        auto settleable = Completion<kanban::GetBoardResult>::makeSettleable(&owner);
        switch (answer) {
            case Answer::Accept:
                settleable.second.resolve(kanban::GetBoardResult{});
                break;
            case Answer::Refuse:
                settleable.second.reject(std::make_exception_ptr(kanban::Conflict{"column is at its WIP limit"}));
                break;
            case Answer::Hold:
                held.push_back(std::move(settleable.second));
                break;
            default:
                break;
        }
        return std::move(settleable.first);
    }
};

[[nodiscard]] kanban::MoveTaskPosition move(std::string opId) {
    return kanban::MoveTaskPosition{.taskId = kanban::TaskId{1},
                                    .columnId = kanban::ColumnId{2},
                                    .swimlaneId = kanban::SwimlaneId{3},
                                    .position = 0,
                                    .opId = std::move(opId)};
}

}  // namespace

TEST_CASE("kanban::client::OfflineMoves: a move enqueued while offline waits, then replays once on reconnect",
          "[kanban][client][offline]") {
    kanban::testing::ScopedQueueFile const queueFile{"offline_moves_replay"};
    Harness harness{queueFile.path()};
    harness.goOffline();

    harness.moves->enqueue(move("op-1"));
    REQUIRE(harness.settle([&harness] { return harness.moves->queueDepth() == 1; }));
    CHECK(harness.replayed.empty());

    harness.goOnline();
    REQUIRE(harness.settle([&harness] { return harness.moves->queueDepth() == 0 && harness.drainsThatLanded > 0; }));
    CHECK(harness.replayed == std::vector<std::string>{"op-1"});
    CHECK(harness.moves->deadLettered() == 0);
}

TEST_CASE("kanban::client::OfflineMoves: a move the server keeps refusing is dead-lettered", "[kanban][client][offline]") {
    kanban::testing::ScopedQueueFile const queueFile{"offline_moves_dead_letter"};
    Harness harness{queueFile.path()};
    harness.answer = Harness::Answer::Refuse;
    harness.goOffline();
    harness.moves->enqueue(move("op-refused"));
    REQUIRE(harness.settle([&harness] { return harness.moves->queueDepth() == 1; }));

    for (int flap = 0; flap < 8 && harness.moves->deadLettered() == 0; ++flap) {
        std::size_t const attempts = harness.replayed.size();
        harness.goOnline();
        REQUIRE(harness.settle([&] { return harness.replayed.size() > attempts || harness.moves->deadLettered() > 0; }));
        harness.goOffline();
    }

    REQUIRE(harness.settle([&harness] { return harness.moves->deadLettered() == 1 && harness.moves->queueDepth() == 0; }));
    CHECK(harness.drainsThatLanded == 0);
}

TEST_CASE("kanban::client::OfflineMoves: destroying it while a replay waits returns promptly",
          "[kanban][client][offline]") {
    kanban::testing::ScopedQueueFile const queueFile{"offline_moves_teardown"};
    Harness harness{queueFile.path()};
    harness.answer = Harness::Answer::Hold;
    harness.goOffline();
    harness.moves->enqueue(move("op-held"));
    REQUIRE(harness.settle([&harness] { return harness.moves->queueDepth() == 1; }));
    harness.goOnline();
    REQUIRE(harness.settle([&harness] { return harness.replayed.size() == 1; }));

    auto const started = std::chrono::steady_clock::now();
    harness.moves.reset();
    CHECK(std::chrono::steady_clock::now() - started < 2s);
}

#endif  // MORPH_BUILD_OFFLINE_SQLITE
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'controllers/offline_moves.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/controllers/offline_config.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>

namespace kanban::client {

/// @brief Where a board keeps the moves made while the network is down, and how it decides that it is.
///
/// Honoured only in a build with `MORPH_BUILD_OFFLINE_SQLITE`; elsewhere a board ignores it and sends every move.
struct OfflineConfig {
    /// @brief The SQLite file the queue lives in.
    std::filesystem::path queuePath;
    /// @brief Whether the server is reachable; empty means always. Called on the monitor's own thread.
    std::function<bool()> probe;
    /// @brief How often the probe runs.
    std::chrono::milliseconds probeInterval{5000};
    /// @brief Consecutive failed probes before moves are queued.
    int failureThreshold = 3;
    /// @brief Consecutive good probes before the queue replays.
    int onlineThreshold = 1;
};

}  // namespace kanban::client
```

Create `examples/kanban/app/controllers/offline_moves.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "controllers/offline_config.hpp"

#ifdef MORPH_BUILD_OFFLINE_SQLITE

#include <atomic>
#include <functional>
#include <memory>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/offline/network_monitor.hpp>
#include <morph/offline/reconnect_coordinator.hpp>
#include <morph/offline/sqlite_offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <string>

#include "kanban/dto/board_dto.hpp"

namespace kanban::client {

/// @brief Board moves made while the network is down: a durable queue, the monitor that decides "down", and the
///        reconnect sequence that replays the queue when the network comes back.
///
/// The monitor probes on its own loop. The queue and its drain run on a strand over a private pool, the reconnect
/// sequence and its retry sleeps on the coordinator's strand over the same pool. A replay posts the move to the
/// owner, where the bridge handler belongs, and waits for its outcome on the drain's thread in short slices.
/// What this class shows (`queueDepth`, `deadLettered`) is written on the owner only.
class OfflineMoves {
public:
    /// @brief Dispatches one queued move. Called on the owner.
    using Replay = std::function<morph::async::Completion<GetBoardResult>(MoveTaskPosition)>;

    /// @param runtime The runtime the counters belong to. Borrowed.
    /// @param owner The runtime's owner, where @p replay runs. Borrowed.
    /// @param config Where the queue lives and how the network is probed.
    /// @param replay Dispatches a queued move.
    /// @param onReplayed Called on the owner after a drain that replayed at least one move.
    OfflineMoves(morph::reactive::Runtime& runtime, morph::exec::IExecutor& owner, OfflineConfig const& config,
                 Replay replay, std::function<void()> onReplayed);

    /// @brief Stops the probe and the reconnect sequence and waits for a running drain; a replay that is waiting
    ///        for its outcome gives up within one slice.
    ~OfflineMoves();

    OfflineMoves(OfflineMoves const&) = delete;
    OfflineMoves& operator=(OfflineMoves const&) = delete;
    OfflineMoves(OfflineMoves&&) = delete;
    OfflineMoves& operator=(OfflineMoves&&) = delete;

    /// @brief Whether the monitor last saw the network. Any thread.
    /// @return False between the monitor's offline and online transitions.
    [[nodiscard]] bool online() const noexcept { return _online->load(); }

    /// @brief Stores @p action for replay, deduplicated by its op id. On the owner.
    /// @param action The move, its op id already minted.
    void enqueue(MoveTaskPosition const& action);

    /// @brief Moves waiting in the queue. Tracked.
    /// @return The depth as last read.
    [[nodiscard]] int queueDepth() const { return _depth.get(); }

    /// @brief Moves dropped after the server refused them through their whole retry budget. Tracked.
    /// @return The running total.
    [[nodiscard]] int deadLettered() const { return _deadLettered.get(); }

    /// @brief Reconnect drains that have finished, whatever they replayed. Tracked.
    /// @return The running total.
    [[nodiscard]] int drainsCompleted() const { return _drains.get(); }

private:
    void runReplay(morph::async::CallbackToken const& token);
    [[nodiscard]] morph::offline::ReplayOutcome replayOne(std::string const& payload,
                                                          morph::async::CallbackToken const& token);
    void refreshDepth();

    morph::exec::IExecutor* _owner;
    Replay _replay;
    std::function<void()> _onReplayed;
    morph::reactive::Signal<int> _depth;
    morph::reactive::Signal<int> _deadLettered;
    morph::reactive::Signal<int> _drains;
    std::shared_ptr<std::atomic<bool>> _online = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> _stopping = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _pool;
    std::unique_ptr<morph::exec::OwnerStrand> _queueOwner;
    std::unique_ptr<morph::offline::SqliteOfflineQueue> _queue;
    std::unique_ptr<morph::offline::ReconnectCoordinator> _coordinator;
    std::unique_ptr<morph::offline::SyncWorker> _worker;
    std::unique_ptr<morph::offline::NetworkMonitor> _monitor;
    // Last member, so it is destroyed first: work posted to the owner after teardown finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace kanban::client

#endif  // MORPH_BUILD_OFFLINE_SQLITE
```

Create `examples/kanban/app/controllers/offline_moves.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/offline_moves.hpp"

#ifdef MORPH_BUILD_OFFLINE_SQLITE

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <glaze/glaze.hpp>
#include <thread>
#include <utility>

namespace kanban::client {

namespace {

// How long a queued move may take to come back from the bridge before its replay counts as refused.
constexpr std::chrono::milliseconds kReplayBudget{10000};
// How often a waiting replay, or a retry sleep, looks at the stop flag.
constexpr std::chrono::milliseconds kWaitSlice{20};
// One thread for the queue's strand, one for the reconnect sequence and its retry sleeps.
constexpr std::size_t kOfflineThreads = 2;

void sleepUnlessStopping(std::atomic<bool> const& stopping, std::chrono::milliseconds duration) {
    auto const until = std::chrono::steady_clock::now() + duration;
    while (!stopping.load()) {
        auto const now = std::chrono::steady_clock::now();
        if (now >= until) {
            return;
        }
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(kWaitSlice, until - now));
    }
}

}  // namespace

OfflineMoves::OfflineMoves(morph::reactive::Runtime& runtime, morph::exec::IExecutor& owner,
                           OfflineConfig const& config, Replay replay, std::function<void()> onReplayed)
    : _owner{&owner},
      _replay{std::move(replay)},
      _onReplayed{std::move(onReplayed)},
      _depth{runtime, 0},
      _deadLettered{runtime, 0},
      _drains{runtime, 0},
      _pool{std::make_unique<morph::exec::ThreadPoolExecutor>(kOfflineThreads)},
      _queueOwner{std::make_unique<morph::exec::OwnerStrand>(*_pool)},
      _queue{std::make_unique<morph::offline::SqliteOfflineQueue>(*_queueOwner, config.queuePath)} {
    morph::async::CallbackToken const token = _lifetime.token();
    _coordinator = std::make_unique<morph::offline::ReconnectCoordinator>(
        morph::offline::ReconnectCoordinator::Deps{
            .tryReconnect = [] { return true; },
            .activatePrimary = [] {},
            .activateLocal = [] {},
            .bindContext = [] {},
            .replay = [this, token] { runReplay(token); },
            .shouldContinue = [online = _online, stopping = _stopping] { return online->load() && !stopping->load(); },
            .sleep = [stopping = _stopping](std::chrono::milliseconds duration) {
                sleepUnlessStopping(*stopping, duration);
            },
        },
        *_pool);
    _worker = std::make_unique<morph::offline::SyncWorker>(
        *_queueOwner, *_queue,
        morph::offline::SyncWorker::DetailedReplayFunction{
            [this, token](std::string const& payload) { return replayOne(payload, token); }},
        [this, token](morph::offline::QueueItem const&) {
            _owner->post(token.guard([this] {
                _deadLettered.set(_deadLettered.peek() + 1);
                refreshDepth();
            }));
        });
    morph::offline::ReconnectCoordinator* const coordinator = _coordinator.get();
    _monitor = std::make_unique<morph::offline::NetworkMonitor>(
        config.probe ? config.probe : morph::offline::NetworkMonitor::ProbeFunction{[] { return true; }},
        [online = _online, coordinator] {
            online->store(false);
            coordinator->onOffline();
        },
        [online = _online, coordinator] {
            online->store(true);
            static_cast<void>(coordinator->onOnline(coordinator->strand()));
        },
        morph::offline::NetworkMonitor::Config{.probeInterval = config.probeInterval,
                                               .failureThreshold = config.failureThreshold,
                                               .onlineThreshold = config.onlineThreshold});
    refreshDepth();
}

OfflineMoves::~OfflineMoves() {
    // Order matters: no probe may start a sequence, no sequence may start a drain, and the drain's strand must be
    // closed (which waits for a running drain) before the queue and the worker it uses are destroyed.
    _stopping->store(true);
    _worker->stop();
    _monitor.reset();
    _coordinator.reset();
    _queueOwner->close();
    _worker.reset();
    _queue.reset();
}

void OfflineMoves::enqueue(MoveTaskPosition const& action) {
    morph::offline::IOfflineQueue& queue = *_queue;
    queue.enqueue(*_owner, glz::write_json(action).value_or(std::string{"{}"}), action.opId)
        .then(_lifetime, [this](std::uint64_t const&) { refreshDepth(); })
        .onError(_lifetime, [this](std::exception_ptr const&) { refreshDepth(); });
}

void OfflineMoves::refreshDepth() {
    morph::offline::IOfflineQueue const& queue = *_queue;
    queue.size(*_owner).then(_lifetime, [this](std::size_t const& depth) { _depth.set(static_cast<int>(depth)); });
}

void OfflineMoves::runReplay(morph::async::CallbackToken const& token) {
    // On the coordinator's strand: the drain's answer is delivered back here, and only the owner may touch the
    // counters, so the outcome is posted on.
    _worker->run(_coordinator->strand()).thenDetached([this, token](morph::offline::SyncResult const& result) {
        bool const landed = result.successful > 0;
        _owner->post(token.guard([this, landed] {
            refreshDepth();
            _drains.set(_drains.peek() + 1);
            if (landed) {
                _onReplayed();
            }
        }));
    });
}

morph::offline::ReplayOutcome OfflineMoves::replayOne(std::string const& payload,
                                                      morph::async::CallbackToken const& token) {
    MoveTaskPosition action;
    if (glz::read_json(action, payload)) {
        return morph::offline::ReplayOutcome::Rejected;
    }
    auto settled = std::make_shared<std::promise<bool>>();
    std::future<bool> outcome = settled->get_future();
    _owner->post(token.guard([this, action = std::move(action), settled] {
        _replay(action)
            .thenDetached([settled](GetBoardResult const&) { settled->set_value(true); })
            .onErrorDetached([settled](std::exception_ptr const&) { settled->set_value(false); });
    }));
    auto const deadline = std::chrono::steady_clock::now() + kReplayBudget;
    while (outcome.wait_for(kWaitSlice) != std::future_status::ready) {
        if (_stopping->load() || std::chrono::steady_clock::now() >= deadline) {
            // The move may have reached the server: "unknown" must be Rejected, never Undelivered, or a poisonous
            // payload is retried forever (sync_worker.hpp, ReplayOutcome).
            return morph::offline::ReplayOutcome::Rejected;
        }
    }
    try {
        return outcome.get() ? morph::offline::ReplayOutcome::Succeeded : morph::offline::ReplayOutcome::Rejected;
    } catch (std::future_error const&) {
        // The owner dropped the posted dispatch because this object was being torn down: nothing was sent.
        return morph::offline::ReplayOutcome::Undelivered;
    }
}

}  // namespace kanban::client

#endif  // MORPH_BUILD_OFFLINE_SQLITE
```

In `examples/kanban/CMakeLists.txt`'s `if(MORPH_BUILD_OFFLINE_SQLITE)` block add:

```cmake
    if(TARGET ladder_kanban_app)
        target_compile_definitions(ladder_kanban_app PUBLIC MORPH_BUILD_OFFLINE_SQLITE)
        target_link_libraries(ladder_kanban_app PUBLIC morph::offline_sqlite)
    endif()
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client][offline]"
```

Expected: PASS, 3 cases (the build/all configure has `MORPH_BUILD_OFFLINE_SQLITE=ON`).

Mutation check: in `replayOne`'s wait loop drop `_stopping->load() ||`. Expected FAIL in "destroying it while a
replay waits returns promptly" (teardown waits out the 10 s budget). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/controllers/offline_config.hpp examples/kanban/app/controllers/offline_moves.hpp \
        examples/kanban/app/controllers/offline_moves.cpp examples/kanban/tests/client/kanban_client_support.hpp \
        examples/kanban/tests/client/test_offline_moves.cpp examples/kanban/CMakeLists.txt
git commit -m "wip(kanban): OfflineMoves: the durable offline move queue without a GUI event loop

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K6: `BoardController` — attach, board forms, cards, moves and polling

Re-expresses `BoardPresenter` and `BoardBridge` (`openBoard`, `refresh`, the `CreateColumn`/`CreateSwimlane`/
`CreateTask` forms, `moveTask` with a fresh op id per call, `startPolling`/`onEventApplied`, `setMyRole`) and
`BoardView.qml`'s conditionals (lane headers only with more than one lane, the column header text, the per-cell
card lists). The board is one `Query<OpenBoard>` keyed on the open project — re-issuing the keyed attach is how it
refreshes (a handler re-pointed at the same key just executes, `docs/spec/core/shared_instances.md`, "Re-pointing,
not re-keying") — and every board write invalidates it. The activity panel is a `Query<GetActivity>` keyed on
the board's presence. Polling is Part 6's `examples::Poller` over `GetEventsSince`, created when a board attaches
and destroyed when it detaches.

**Files:**
- Create: `examples/kanban/app/controllers/board_controller.hpp`, `examples/kanban/app/controllers/board_controller.cpp`
- Create: `examples/kanban/tests/client/board_support.hpp`
- Test: `examples/kanban/tests/client/test_board_controller.cpp`, `examples/kanban/tests/client/test_board_polling.cpp`,
  `examples/kanban/tests/client/test_board_socket.cpp`

**Interfaces:**
- Consumes: `columnHeader`, `roleLabel`, `activityMeta`, `pendingSyncLine`, `deadLetterLine` (Task K2);
  `morph::examples::Wiring`, `mapCompletion` (Part 6); `forms::FormModel::forAction<A>()`, `FormSession::assign`
  (Part 5); `OfflineConfig`, `OfflineMoves` (Task K5);
  `morph::examples::Poller<BoardEvent, BoardEventId>` and `morph::examples::newUuid()` (Part 6, shapes as stated
  under "What this plan relies on"); `reactive::Query`, `Mutation`, `Computed`, `Effect`, `Runtime::batch`,
  `Runtime::untracked` (Part 1); `testkit/backend_rig.hpp` (`BackendRig`, `Mode::Socket`), `testkit/fault_proxy.hpp`
  (`FaultProxy::start`, `setRequestObserver`, `delayReply`), `testkit/pump.hpp` (`pumpUntil`, `awaitQt`).
- Produces (Tasks K7–K14 use exactly these):
  - Rows: `CardRow{id, columnId, laneId, position, label}`, `ColumnHead{id, name}`, `LaneHead{id, label,
    canAddTask}`, `BoardShape{columns, lanes, key}`, `ActivityRow{index, summary, meta}`, `BoardOptions{offline,
    pollEvery}`.
  - `BoardController(examples::Wiring, BoardOptions = {})`: `open(ProjectId, Role)`, `close()`, `refresh()`, `isOpen()`,
    `loading()`, `board() -> std::optional<GetBoardResult> const&`, `attachedProject()`, `title()`, `roleText()`,
    `shapes() -> std::vector<BoardShape>`, `headerOf(std::int64_t columnId)`, `cardsIn(std::int64_t columnId,
    std::optional<std::int64_t> laneId)`, `activity()`, `errorText()`, `eventsSeen()`, `online()`, `queueDepth()`,
    `deadLettered()`, `syncRuns()`, `syncText()`, `deadLetterText()`, `moveTask(TaskId, ColumnId, SwimlaneId, std::int64_t)`,
    `dropAtEnd(TaskId, ColumnId, std::optional<std::int64_t> laneId)`, `movesPending()`, `lastMoveOpId()`, static
    `taskOf(std::variant<std::int64_t, std::string> const&) -> std::optional<TaskId>`, `createColumnForm()`,
    `createSwimlaneForm()`, `createTaskForm()`, `openNewTask(ColumnId, SwimlaneId)`, `closeNewTask()`,
    `newTaskOpen()`, `newTaskTitle()`, `handler()`.
  - Test support: `kanban::testing::SeededBoard{project, todo, done, lane, task}`, `columnNamed(board, name)`,
    `seedBoard(client, board, project, doneWipLimit = 0)`.

- [ ] **Step 1: Write the failing tests**

Create `examples/kanban/tests/client/board_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>

#include "controllers/board_controller.hpp"
#include "kanban_client_support.hpp"

namespace kanban::testing {

/// The ids a seeded board's tests act on.
struct SeededBoard {
    kanban::ProjectId project;
    std::int64_t todo = 0;
    std::int64_t done = 0;
    std::int64_t lane = 0;
    std::int64_t task = 0;
};

/// The id of the column called @p name.
[[nodiscard]] inline std::int64_t columnNamed(kanban::GetBoardResult const& board, std::string_view name) {
    for (kanban::ColumnView const& column : board.columns) {
        if (column.name == name && column.id.hasValue()) {
            return *column.id;
        }
    }
    FAIL("the board has no column named " << name);
    return 0;
}

/// Opens @p project on @p board and gives it, through the board's own forms, two columns ("To Do", and "Done"
/// with @p doneWipLimit), one swimlane and one task "Fix bug" in To Do.
template <typename Client>
[[nodiscard]] SeededBoard seedBoard(Client& client, kanban::client::BoardController& board, kanban::ProjectId project,
                                    std::int64_t doneWipLimit = 0) {
    board.open(project, kanban::Role::Manager);
    REQUIRE(client.settle([&board] { return board.board().has_value(); }));
    submitForm(client, board.createColumnForm(), R"({"name":"To Do"})");
    submitForm(client, board.createColumnForm(),
               R"({"name":"Done","wipLimit":)" + std::to_string(doneWipLimit) + "}");
    submitForm(client, board.createSwimlaneForm(), R"({"name":"Default"})");
    REQUIRE(client.settle([&board] {
        return board.board() && board.board()->columns.size() == 2 && board.board()->swimlanes.size() == 1;
    }));
    SeededBoard seeded{.project = project,
                       .todo = columnNamed(*board.board(), "To Do"),
                       .done = columnNamed(*board.board(), "Done"),
                       .lane = *board.board()->swimlanes.front().id};
    board.openNewTask(kanban::ColumnId{seeded.todo}, kanban::SwimlaneId{seeded.lane});
    submitForm(client, board.createTaskForm(), R"({"title":"Fix bug"})");
    REQUIRE(client.settle([&] { return board.cardsIn(seeded.todo, seeded.lane).size() == 1; }));
    seeded.task = board.cardsIn(seeded.todo, seeded.lane).front().id;
    return seeded;
}

}  // namespace kanban::testing
```

Create `examples/kanban/tests/client/test_board_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "board_support.hpp"
#include "controllers/board_controller.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"

using kanban::ColumnId;
using kanban::SwimlaneId;
using kanban::TaskId;
using kanban::client::BoardController;
using kanban::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

TEST_CASE("kanban::client::BoardController: open attaches and reports the board's empty initial state",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    CHECK_FALSE(board.isOpen());
    CHECK(board.title() == "Board");
    CHECK(board.roleText().empty());
    CHECK(board.shapes().empty());

    board.open(project, kanban::Role::Manager);

    CHECK(board.isOpen());
    REQUIRE(client.settle([&board] { return board.board().has_value(); }));
    CHECK(board.title() == "Sprint Board");
    CHECK(board.roleText() == "role: Manager");
    CHECK(board.attachedProject() == std::optional<kanban::ProjectId>{project});
    REQUIRE(board.shapes().size() == 1);
    CHECK(board.shapes().front().columns.empty());
    REQUIRE(board.shapes().front().lanes.size() == 1);
    CHECK_FALSE(board.shapes().front().lanes.front().canAddTask);
    CHECK(board.errorText().empty());
}

TEST_CASE("kanban::client::BoardController: the column, swimlane and task forms populate the board",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);

    auto const shape = board.shapes().front();
    REQUIRE(shape.columns.size() == 2);
    CHECK(shape.columns.at(0).name == "To Do");
    CHECK(shape.columns.at(1).name == "Done");
    REQUIRE(shape.lanes.size() == 1);
    CHECK(shape.lanes.front().label.empty());
    CHECK(shape.lanes.front().canAddTask);
    CHECK(board.headerOf(seeded.todo) == "To Do  (1)");
    auto const cards = board.cardsIn(seeded.todo, seeded.lane);
    REQUIRE(cards.size() == 1);
    CHECK(cards.front().label == "Fix bug");
    CHECK_FALSE(board.newTaskOpen());

    board.openNewTask(ColumnId{seeded.done}, SwimlaneId{seeded.lane});
    CHECK(board.newTaskOpen());
    CHECK(board.newTaskTitle() == "New task in Done");
    board.closeNewTask();
    CHECK_FALSE(board.newTaskOpen());
}

TEST_CASE("kanban::client::BoardController: moveTask moves a card, every move with a fresh op id",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);

    board.moveTask(TaskId{seeded.task}, ColumnId{seeded.done}, SwimlaneId{seeded.lane}, 0);
    std::string const first = board.lastMoveOpId();
    REQUIRE(client.settle([&] { return board.cardsIn(seeded.done, seeded.lane).size() == 1 && !board.movesPending(); }));

    board.moveTask(TaskId{seeded.task}, ColumnId{seeded.todo}, SwimlaneId{seeded.lane}, 0);
    std::string const second = board.lastMoveOpId();
    REQUIRE(client.settle([&] { return board.cardsIn(seeded.todo, seeded.lane).size() == 1 && !board.movesPending(); }));

    CHECK(first.size() == 36);
    CHECK(second.size() == 36);
    CHECK(first != second);
    CHECK(board.errorText().empty());
}

TEST_CASE("kanban::client::BoardController: dropAtEnd places the card after the cards already there",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);
    board.openNewTask(ColumnId{seeded.done}, SwimlaneId{seeded.lane});
    kanban::testing::submitForm(client, board.createTaskForm(), R"({"title":"Ship it"})");
    REQUIRE(client.settle([&] { return board.cardsIn(seeded.done, seeded.lane).size() == 1; }));

    board.dropAtEnd(TaskId{seeded.task}, ColumnId{seeded.done}, std::nullopt);

    REQUIRE(client.settle([&] { return board.cardsIn(seeded.done, seeded.lane).size() == 2; }));
    auto const cards = board.cardsIn(seeded.done, seeded.lane);
    CHECK(cards.at(0).label == "Ship it");
    CHECK(cards.at(1).label == "Fix bug");
}

TEST_CASE("kanban::client::BoardController: a move the server refuses reports and leaves the card",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project, /*doneWipLimit=*/1);
    board.openNewTask(ColumnId{seeded.done}, SwimlaneId{seeded.lane});
    kanban::testing::submitForm(client, board.createTaskForm(), R"({"title":"Blocker"})");
    REQUIRE(client.settle([&] { return board.cardsIn(seeded.done, seeded.lane).size() == 1; }));

    board.moveTask(TaskId{seeded.task}, ColumnId{seeded.done}, SwimlaneId{seeded.lane}, 0);

    REQUIRE(client.settle([&board] { return !board.errorText().empty(); }));
    CHECK(board.cardsIn(seeded.todo, seeded.lane).size() == 1);
}

TEST_CASE("kanban::client::BoardController: a disengaged project id surfaces as an error, not a board",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    BoardController board{client.wiring()};

    board.open(kanban::ProjectId{}, kanban::Role::Viewer);

    REQUIRE(client.settle([&board] { return !board.errorText().empty(); }));
    CHECK_FALSE(board.board().has_value());
}

TEST_CASE("kanban::client::BoardController::taskOf accepts positive integer keys only", "[kanban][client]") {
    using Key = std::variant<std::int64_t, std::string>;
    CHECK(BoardController::taskOf(Key{std::int64_t{7}}) == std::optional<TaskId>{TaskId{7}});
    CHECK_FALSE(BoardController::taskOf(Key{std::int64_t{0}}).has_value());
    CHECK_FALSE(BoardController::taskOf(Key{std::string{"7"}}).has_value());
}
```

Create `examples/kanban/tests/client/test_board_polling.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "board_support.hpp"
#include "controllers/board_controller.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"

using kanban::ColumnId;
using kanban::SwimlaneId;
using kanban::TaskId;
using kanban::client::BoardController;
using kanban::testing::LocalClient;
using morph::ladder::testkit::DbFixture;
using namespace std::chrono_literals;

TEST_CASE("kanban::client::BoardController: another client's move reaches an open board on the next poll",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController mover{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, mover, project);
    BoardController watcher{client.wiring()};
    watcher.open(project, kanban::Role::Member);
    // The first poll replays every event since the board began, so the watcher refreshes once on its own; wait
    // until that refresh has landed before anything else happens.
    REQUIRE(client.settle([&watcher] { return watcher.eventsSeen() > 0 && !watcher.loading(); }));
    REQUIRE(watcher.cardsIn(seeded.todo, seeded.lane).size() == 1);

    mover.moveTask(TaskId{seeded.task}, ColumnId{seeded.done}, SwimlaneId{seeded.lane}, 0);
    REQUIRE(client.settle([&] { return mover.cardsIn(seeded.done, seeded.lane).size() == 1; }));
    CHECK(watcher.cardsIn(seeded.todo, seeded.lane).size() == 1);

    client.scheduler.advance(3000ms);

    REQUIRE(client.settle([&] { return watcher.cardsIn(seeded.done, seeded.lane).size() == 1; }));
    CHECK(watcher.cardsIn(seeded.todo, seeded.lane).empty());
}

TEST_CASE("kanban::client::BoardController: closing the board stops polling", "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    board.open(project, kanban::Role::Manager);
    REQUIRE(client.settle([&board] { return board.board().has_value(); }));
    CHECK(client.scheduler.pendingTimers() > 0);

    board.close();

    REQUIRE(client.settle([&board] { return !board.board().has_value(); }));
    CHECK(client.scheduler.pendingTimers() == 0);
    CHECK(board.eventsSeen() == 0);
}
```

Create `examples/kanban/tests/client/test_board_socket.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The board over a real socket whose attach reply is held back: the board must still appear once the reply lands,
// with nothing re-issued in between.

#include <QUrl>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <optional>
#include <string>

#include "app/wiring.hpp"
#include "controllers/board_controller.hpp"
#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban_client_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/fault_proxy.hpp"
#include "testkit/pump.hpp"

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::FaultProxy;
using morph::ladder::testkit::Mode;
using namespace std::chrono_literals;

TEST_CASE("kanban::client::BoardController: a stalled Socket-mode attach still reports the board once it lands",
          "[kanban][client][socket]") {
    DbFixture const fixture;
    auto const authorizer = std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kanban::testing::kSecret},
                                                                             morph::session::hmacSha256);
    BackendRig rig{Mode::Socket, 1, authorizer};
    rig.bridge(0).setDefaultSession(kanban::testing::signedContext("alice"));
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> creator{rig.bridge(0), rig.executor()};
    auto const project =
        morph::ladder::testkit::awaitQt(creator.execute(kanban::CreateProject{.name = "Sprint Board"})).id;

    FaultProxy proxy{rig.url()};
    QUrl const proxyUrl = proxy.start();
    auto backend = std::make_unique<morph::qt::QtWebSocketBackend>(
        proxyUrl, std::nullopt, morph::qt::QtWebSocketBackend::Config{.reconnectEnabled = false});
    REQUIRE(backend->waitForConnected());
    morph::qt::QtExecutor owner;
    morph::bridge::Bridge bridge{std::move(backend), owner};
    bridge.setDefaultSession(kanban::testing::signedContext("alice"));
    proxy.setRequestObserver([](std::uint64_t callId, FaultProxy& self) { self.delayReply(callId, 300ms); });
    morph::reactive::Runtime runtime{owner};
    morph::reactive::testing::ManualScheduler scheduler;
    kanban::client::BoardController board{
        morph::examples::Wiring{.runtime = runtime, .scheduler = scheduler, .bridge = bridge, .callbacks = owner}};

    board.open(project, kanban::Role::Manager);

    REQUIRE(morph::ladder::testkit::pumpUntil(
        [&board] { return board.board().has_value() || !board.errorText().empty(); }, 5000ms));
    CHECK(board.errorText().empty());
    REQUIRE(board.board().has_value());
    CHECK(board.board()->name == "Sprint Board");
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'controllers/board_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/controllers/board_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "app/poller.hpp"
#include "app/wiring.hpp"
#include "controllers/offline_config.hpp"
#include "kanban/models/board_model.hpp"

namespace kanban::client {

/// @brief One card on the board.
struct CardRow {
    /// @brief The task's id; also the card's drag key.
    std::int64_t id = 0;
    /// @brief The column it is in.
    std::int64_t columnId = 0;
    /// @brief The swimlane it is in.
    std::int64_t laneId = 0;
    /// @brief Its position within that cell.
    std::int64_t position = 0;
    /// @brief The title, with its tags in brackets when it has any.
    std::string label;
    /// @brief Rows compare by value, so an unchanged card keeps its widget.
    bool operator==(CardRow const&) const = default;
};

/// @brief One column of the board's grid.
struct ColumnHead {
    /// @brief The column's id.
    std::int64_t id = 0;
    /// @brief Its name.
    std::string name;
    /// @brief Compared by value.
    bool operator==(ColumnHead const&) const = default;
};

/// @brief One swimlane row of the board's grid.
struct LaneHead {
    /// @brief The swimlane's id; empty while the board has no swimlane, when the one row holds every card.
    std::optional<std::int64_t> id;
    /// @brief The lane's name when the board has more than one lane, else empty.
    std::string label;
    /// @brief Whether a task can be created in this row (a task needs a swimlane).
    bool canAddTask = false;
    /// @brief Compared by value.
    bool operator==(LaneHead const&) const = default;
};

/// @brief The grid's structure: which columns and lanes exist. Cards are not part of it.
struct BoardShape {
    /// @brief The columns, left to right.
    std::vector<ColumnHead> columns;
    /// @brief The lanes, top to bottom; never empty while a board is open.
    std::vector<LaneHead> lanes;
    /// @brief Changes exactly when the columns or lanes change; the view rebuilds its grid on it.
    std::string key;
    /// @brief Compared by value.
    bool operator==(BoardShape const&) const = default;
};

/// @brief One entry in the activity panel.
struct ActivityRow {
    /// @brief The entry's place in the stream; its key.
    std::int64_t index = 0;
    /// @brief What happened.
    std::string summary;
    /// @brief Who did it, and with which action.
    std::string meta;
    /// @brief Compared by value.
    bool operator==(ActivityRow const&) const = default;
};

/// @brief How a board runs besides its wiring.
struct BoardOptions {
    /// @brief Engaged: moves made while the network is down are queued and replayed (builds with
    ///        `MORPH_BUILD_OFFLINE_SQLITE` only).
    std::optional<OfflineConfig> offline;
    /// @brief How often an open board polls for other clients' changes.
    std::chrono::milliseconds pollEvery{3000};
};

class OfflineMoves;

/// @brief The board screen: attach, the board and its activity, the board forms, moving cards, and polling for
///        other clients' changes.
class BoardController {
public:
    /// @param wiring The runtime, scheduler, bridge and owner this controller uses.
    /// @param options Offline queue and poll period.
    explicit BoardController(morph::examples::Wiring wiring, BoardOptions options = {});
    ~BoardController();
    BoardController(BoardController const&) = delete;
    BoardController& operator=(BoardController const&) = delete;
    BoardController(BoardController&&) = delete;
    BoardController& operator=(BoardController&&) = delete;

    /// @brief Attaches to @p project's board and starts showing it.
    /// @param project The project.
    /// @param myRole The caller's role there, as the project list reported it.
    void open(ProjectId project, Role myRole);
    /// @brief Detaches: the board, its activity and its polling go idle.
    void close();
    /// @brief Fetches the board (and with it the activity) again.
    void refresh();
    /// @brief Whether a project is open. Tracked.
    /// @return True between `open` and `close`.
    [[nodiscard]] bool isOpen() const { return _project.get().has_value(); }
    /// @brief Whether the board is being fetched. Tracked.
    /// @return True while a fetch is in flight.
    [[nodiscard]] bool loading() const { return _open.pending(); }
    /// @brief The attached board. Tracked.
    /// @return Its state, kept while a refresh is in flight; empty while closed.
    [[nodiscard]] std::optional<GetBoardResult> const& board() const { return _open.value(); }
    /// @brief The project whose board is attached. Tracked, equality-gated.
    /// @return The project, or empty.
    [[nodiscard]] std::optional<ProjectId> attachedProject() const { return _attached.get(); }
    /// @brief The board's title. Tracked.
    /// @return The project name, or `"Board"` before one is attached.
    [[nodiscard]] std::string title() const;
    /// @brief The caller's role line. Tracked.
    /// @return `"role: <role>"` while open, else empty.
    [[nodiscard]] std::string roleText() const;
    /// @brief The grid's structure, as a list of zero or one shapes so a view can key on it. Tracked.
    /// @return One shape while a board is attached, else none.
    [[nodiscard]] std::vector<BoardShape> shapes() const;
    /// @brief A column's header text. Tracked.
    /// @param columnId The column.
    /// @return Its header, or empty when it is gone.
    [[nodiscard]] std::string headerOf(std::int64_t columnId) const;
    /// @brief The cards in one cell, by position. Tracked.
    /// @param columnId The column.
    /// @param laneId The swimlane; empty matches every lane.
    /// @return The cards.
    [[nodiscard]] std::vector<CardRow> cardsIn(std::int64_t columnId, std::optional<std::int64_t> laneId) const;
    /// @brief The activity stream, oldest first. Tracked.
    /// @return One row per entry.
    [[nodiscard]] std::vector<ActivityRow> activity() const;
    /// @brief The first failure among attach, moves and activity, else why polling stopped. Tracked.
    /// @return The message, or empty.
    [[nodiscard]] std::string errorText() const;
    /// @brief How many events the open board's poller has applied. Tracked while the poller lives.
    /// @return The count, zero without a poller.
    [[nodiscard]] std::size_t eventsSeen() const;
    /// @brief Whether moves are sent (true) or queued (false). Any thread.
    /// @return True without an offline queue.
    [[nodiscard]] bool online() const;
    /// @brief Moves waiting in the offline queue. Tracked.
    /// @return The depth; zero without a queue.
    [[nodiscard]] int queueDepth() const;
    /// @brief Moves the server refused through their whole retry budget. Tracked.
    /// @return The total; zero without a queue.
    [[nodiscard]] int deadLettered() const;
    /// @brief Reconnect drains of the offline queue that have finished. Tracked.
    /// @return The total; zero without a queue.
    [[nodiscard]] int syncRuns() const;
    /// @brief The pending-sync banner. Tracked.
    /// @return The line, or empty.
    [[nodiscard]] std::string syncText() const;
    /// @brief The dead-letter banner. Tracked.
    /// @return The line, or empty.
    [[nodiscard]] std::string deadLetterText() const;
    /// @brief Moves @p task to @p position in (@p column, @p lane) under a fresh op id: sent now, or queued while
    ///        offline.
    /// @param task The task.
    /// @param column The destination column.
    /// @param lane The destination swimlane.
    /// @param position The destination position.
    void moveTask(TaskId task, ColumnId column, SwimlaneId lane, std::int64_t position);
    /// @brief Moves @p task to the end of (@p column, @p laneId).
    /// @param task The task.
    /// @param column The destination column.
    /// @param laneId The destination swimlane; empty keeps the task's own.
    void dropAtEnd(TaskId task, ColumnId column, std::optional<std::int64_t> laneId);
    /// @brief Whether a sent move is in flight. Tracked.
    /// @return True while any is.
    [[nodiscard]] bool movesPending() const { return _move.pending(); }
    /// @brief The op id the last move was minted with.
    /// @return The id; empty before the first move.
    [[nodiscard]] std::string const& lastMoveOpId() const noexcept { return _lastMoveOpId; }
    /// @brief The task a drag key names.
    /// @param key A drag key (`ui::Key`'s alternatives).
    /// @return The task for a positive integer key, else empty.
    [[nodiscard]] static std::optional<TaskId> taskOf(std::variant<std::int64_t, std::string> const& key) noexcept;
    /// @brief The `CreateColumn` form.
    /// @return The form session.
    [[nodiscard]] morph::forms::FormSession& createColumnForm() noexcept { return _createColumn; }
    /// @brief The `CreateSwimlane` form.
    /// @return The form session.
    [[nodiscard]] morph::forms::FormSession& createSwimlaneForm() noexcept { return _createSwimlane; }
    /// @brief The `CreateTask` form; `openNewTask` fills its hidden ids.
    /// @return The form session.
    [[nodiscard]] morph::forms::FormSession& createTaskForm() noexcept { return _createTask; }
    /// @brief Opens the new-task dialog for one cell.
    /// @param column The cell's column.
    /// @param lane The cell's swimlane.
    void openNewTask(ColumnId column, SwimlaneId lane);
    /// @brief Closes the new-task dialog.
    void closeNewTask();
    /// @brief Whether the new-task dialog is open. Tracked.
    /// @return True between `openNewTask` and its success or `closeNewTask`.
    [[nodiscard]] bool newTaskOpen() const { return _newTask.get().has_value(); }
    /// @brief The new-task dialog's title. Tracked.
    /// @return `"New task in <column>"`, or `"New task"`.
    [[nodiscard]] std::string newTaskTitle() const;
    /// @brief The board's handler; the rules and task-detail controllers act on the same attached instance.
    /// @return The handler.
    [[nodiscard]] morph::bridge::BridgeHandler<BoardModel, morph::bridge::AllowShared>& handler() noexcept {
        return _handler;
    }

private:
    class Polling;
    struct NewTaskTarget {
        ColumnId column;
        SwimlaneId lane;
        bool operator==(NewTaskTarget const&) const = default;
    };

    [[nodiscard]] morph::async::Completion<std::string> submitBoardForm(std::string_view actionType, std::string body,
                                                                        std::function<void()> after);
    [[nodiscard]] morph::async::Completion<morph::examples::PollPage<BoardEvent, BoardEventId>> eventsSince(
        BoardEventId cursor);
    [[nodiscard]] std::optional<std::int64_t> laneOf(std::int64_t taskId) const;
    void followAttach();

    morph::examples::Wiring _wiring;
    BoardOptions _options;
    morph::bridge::BridgeHandler<BoardModel, morph::bridge::AllowShared> _handler;
    morph::reactive::Signal<std::optional<ProjectId>> _project;
    morph::reactive::Signal<Role> _role;
    morph::reactive::Signal<std::optional<NewTaskTarget>> _newTask;
    morph::reactive::Signal<std::string> _pollingStopped;
    morph::reactive::Query<OpenBoard> _open;
    morph::reactive::Computed<std::optional<ProjectId>> _attached;
    morph::reactive::Query<GetActivity> _activity;
    morph::reactive::Mutation<MoveTaskPosition> _move;
    morph::forms::FormSession _createColumn;
    morph::forms::FormSession _createSwimlane;
    morph::forms::FormSession _createTask;
    std::unique_ptr<OfflineMoves> _offline;
    std::optional<ProjectId> _pollingProject;
    std::unique_ptr<Polling> _polling;
    std::unique_ptr<morph::reactive::Effect> _follow;
    std::string _lastMoveOpId;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace kanban::client
```

Create `examples/kanban/app/controllers/board_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/board_controller.hpp"

#include <algorithm>
#include <exception>
#include <morph/forms/engine/field_model.hpp>
#include <utility>

#include "app/completion_map.hpp"
#include "app/uuid.hpp"
#include "support/board_format.hpp"

#ifdef MORPH_BUILD_OFFLINE_SQLITE
#include "controllers/offline_moves.hpp"
#else
namespace kanban::client {
// Stands in for the offline queue in a build without MORPH_BUILD_OFFLINE_SQLITE; never constructed.
class OfflineMoves {};
}  // namespace kanban::client
#endif

namespace kanban::client {

namespace {

[[nodiscard]] std::string cardLabel(TaskView const& task) {
    if (task.tags.empty()) {
        return task.title;
    }
    std::string label = task.title + "  [";
    for (std::size_t i = 0; i < task.tags.size(); ++i) {
        label += (i == 0 ? "" : ", ") + task.tags.at(i);
    }
    return label + "]";
}

}  // namespace

// Polls the attached board for events and refreshes the board when new ones arrive. One exists per attached
// board; it starts from the beginning of the board's event stream, so its first poll refreshes once. The poller
// applies a page's events one by one; counting them and refreshing from a separate effect refreshes once per page,
// not once per event.
class BoardController::Polling {
public:
    explicit Polling(BoardController& board)
        : _board{&board},
          _seen{board._wiring.runtime, std::size_t{0}},
          _poller{board._wiring.runtime,
                  board._wiring.scheduler,
                  [&board](BoardEventId const& cursor) { return board.eventsSince(cursor); },
                  BoardEventId{},
                  [this](BoardEvent const&) { _seen.set(_seen.peek() + 1); },
                  morph::examples::PollerOptions{.interval = board._options.pollEvery}},
          _onEvents{board._wiring.runtime, [this] { onEvents(); }} {}

    [[nodiscard]] std::size_t seen() const { return _seen.get(); }

private:
    void onEvents() {
        std::size_t const count = _seen.get();
        std::exception_ptr const failure = _poller.stoppedBy();
        _board->_wiring.runtime.untracked([&] {
            if (count > _refreshedAt) {
                _refreshedAt = count;
                _board->refresh();
            }
            if (failure != nullptr) {
                _board->_pollingStopped.set("polling stopped: " + morph::reactive::errorMessage(failure));
            }
        });
    }

    BoardController* _board;
    morph::reactive::Signal<std::size_t> _seen;
    std::size_t _refreshedAt = 0;
    morph::examples::Poller<BoardEvent, BoardEventId> _poller;
    morph::reactive::Effect _onEvents;
};

BoardController::BoardController(morph::examples::Wiring wiring, BoardOptions options)
    : _wiring{wiring},
      _options{std::move(options)},
      _handler{wiring.bridge, &wiring.callbacks},
      _project{wiring.runtime, std::nullopt},
      _role{wiring.runtime, Role::Viewer},
      _newTask{wiring.runtime, std::nullopt},
      _pollingStopped{wiring.runtime, std::string{}},
      _open{wiring.runtime, _handler,
            [this]() -> std::optional<OpenBoard> {
                auto const& project = _project.get();
                if (!project) {
                    return std::nullopt;
                }
                return OpenBoard{.projectId = *project};
            }},
      _attached{wiring.runtime,
                [this]() -> std::optional<ProjectId> {
                    auto const& current = _open.value();
                    if (!current) {
                        return std::nullopt;
                    }
                    return current->projectId;
                }},
      _activity{wiring.runtime, _handler,
                [this]() -> std::optional<GetActivity> {
                    if (!_open.value()) {
                        return std::nullopt;
                    }
                    return GetActivity{};
                }},
      _move{wiring.runtime, _handler, morph::reactive::MutationOptions{.invalidates = {&_open}}},
      _createColumn{wiring.runtime, morph::forms::FormModel::forAction<CreateColumn>(),
                    [this](std::string_view actionType, std::string body) {
                        return submitBoardForm(actionType, std::move(body), [this] { _createColumn.reset(); });
                    },
                    {}},
      _createSwimlane{wiring.runtime, morph::forms::FormModel::forAction<CreateSwimlane>(),
                      [this](std::string_view actionType, std::string body) {
                          return submitBoardForm(actionType, std::move(body), [this] { _createSwimlane.reset(); });
                      },
                      {}},
      _createTask{wiring.runtime, morph::forms::FormModel::forAction<CreateTask>(),
                  [this](std::string_view actionType, std::string body) {
                      return submitBoardForm(actionType, std::move(body), [this] { _newTask.set(std::nullopt); });
                  },
                  {}} {
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    if (_options.offline) {
        _offline = std::make_unique<OfflineMoves>(
            _wiring.runtime, _wiring.callbacks, *_options.offline,
            [this](MoveTaskPosition action) { return _handler.execute(std::move(action)); }, [this] { refresh(); });
    }
#endif
    _follow = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this] { followAttach(); });
}

BoardController::~BoardController() = default;

void BoardController::open(ProjectId project, Role myRole) {
    _wiring.runtime.batch([&] {
        _role.set(myRole);
        _pollingStopped.set(std::string{});
        _project.set(project);
    });
}

void BoardController::close() {
    _wiring.runtime.batch([&] {
        _newTask.set(std::nullopt);
        _project.set(std::nullopt);
    });
}

void BoardController::refresh() { _open.refetch(); }

std::string BoardController::title() const {
    auto const& current = _open.value();
    return current ? current->name : std::string{"Board"};
}

std::string BoardController::roleText() const { return isOpen() ? "role: " + roleLabel(_role.get()) : std::string{}; }

std::vector<BoardShape> BoardController::shapes() const {
    auto const& current = _open.value();
    if (!current) {
        return {};
    }
    BoardShape shape;
    shape.key = "columns";
    for (ColumnView const& column : current->columns) {
        if (column.id.hasValue()) {
            shape.columns.push_back(ColumnHead{.id = *column.id, .name = column.name});
            shape.key += ":" + std::to_string(*column.id);
        }
    }
    shape.key += "|lanes";
    bool const named = current->swimlanes.size() > 1;
    for (SwimlaneView const& lane : current->swimlanes) {
        if (lane.id.hasValue()) {
            shape.lanes.push_back(
                LaneHead{.id = *lane.id, .label = named ? lane.name : std::string{}, .canAddTask = true});
            shape.key += ":" + std::to_string(*lane.id);
        }
    }
    if (shape.lanes.empty()) {
        shape.lanes.push_back(LaneHead{.id = std::nullopt, .label = std::string{}, .canAddTask = false});
    }
    return std::vector<BoardShape>{std::move(shape)};
}

std::string BoardController::headerOf(std::int64_t columnId) const {
    auto const& current = _open.value();
    if (current) {
        for (ColumnView const& column : current->columns) {
            if (column.id == ColumnId{columnId}) {
                return columnHeader(column);
            }
        }
    }
    return {};
}

std::vector<CardRow> BoardController::cardsIn(std::int64_t columnId, std::optional<std::int64_t> laneId) const {
    std::vector<CardRow> cards;
    auto const& current = _open.value();
    if (!current) {
        return cards;
    }
    for (TaskView const& task : current->tasks) {
        if (!task.id.hasValue() || task.columnId != ColumnId{columnId}) {
            continue;
        }
        std::int64_t const taskLane = task.swimlaneId.hasValue() ? *task.swimlaneId : 0;
        if (laneId && taskLane != *laneId) {
            continue;
        }
        cards.push_back(CardRow{.id = *task.id,
                                .columnId = columnId,
                                .laneId = taskLane,
                                .position = task.position,
                                .label = cardLabel(task)});
    }
    std::ranges::sort(cards, {}, &CardRow::position);
    return cards;
}

std::vector<ActivityRow> BoardController::activity() const {
    std::vector<ActivityRow> rows;
    auto const& listed = _activity.value();
    if (!listed) {
        return rows;
    }
    for (ActivityEvent const& event : listed->events) {
        rows.push_back(ActivityRow{.index = static_cast<std::int64_t>(rows.size()),
                                   .summary = event.summary,
                                   .meta = activityMeta(event)});
    }
    return rows;
}

std::string BoardController::errorText() const {
    for (std::exception_ptr const& error : {_open.error(), _move.error(), _activity.error()}) {
        if (error != nullptr) {
            return morph::reactive::errorMessage(error);
        }
    }
    return _pollingStopped.get();
}

std::size_t BoardController::eventsSeen() const { return _polling != nullptr ? _polling->seen() : 0; }

bool BoardController::online() const {
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    if (_offline != nullptr) {
        return _offline->online();
    }
#endif
    return true;
}

int BoardController::queueDepth() const {
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    if (_offline != nullptr) {
        return _offline->queueDepth();
    }
#endif
    return 0;
}

int BoardController::deadLettered() const {
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    if (_offline != nullptr) {
        return _offline->deadLettered();
    }
#endif
    return 0;
}

int BoardController::syncRuns() const {
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    if (_offline != nullptr) {
        return _offline->drainsCompleted();
    }
#endif
    return 0;
}

std::string BoardController::syncText() const { return pendingSyncLine(queueDepth()); }

std::string BoardController::deadLetterText() const { return deadLetterLine(deadLettered()); }

void BoardController::moveTask(TaskId task, ColumnId column, SwimlaneId lane, std::int64_t position) {
    // A fresh op id per gesture: MoveTaskPosition is exactly-once per op id, so a reused id would replay the
    // first move's stored result instead of moving.
    MoveTaskPosition action{.taskId = task,
                            .columnId = column,
                            .swimlaneId = lane,
                            .position = position,
                            .opId = morph::examples::newUuid()};
    _lastMoveOpId = action.opId;
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    if (_offline != nullptr && !_offline->online()) {
        _offline->enqueue(action);
        return;
    }
#endif
    _move.run(std::move(action));
}

void BoardController::dropAtEnd(TaskId task, ColumnId column, std::optional<std::int64_t> laneId) {
    if (!task.hasValue() || !column.hasValue()) {
        return;
    }
    std::optional<std::int64_t> const lane = laneId ? laneId : laneOf(*task);
    if (!lane) {
        return;
    }
    std::int64_t position = 0;
    for (CardRow const& card : cardsIn(*column, lane)) {
        if (card.id != *task) {
            ++position;
        }
    }
    moveTask(task, column, SwimlaneId{*lane}, position);
}

std::optional<TaskId> BoardController::taskOf(std::variant<std::int64_t, std::string> const& key) noexcept {
    if (auto const* const taskId = std::get_if<std::int64_t>(&key); taskId != nullptr && *taskId > 0) {
        return TaskId{*taskId};
    }
    return std::nullopt;
}

void BoardController::openNewTask(ColumnId column, SwimlaneId lane) {
    if (!column.hasValue() || !lane.hasValue()) {
        return;
    }
    _createTask.reset();
    _createTask.assign("columnId", std::to_string(*column));
    _createTask.assign("swimlaneId", std::to_string(*lane));
    _newTask.set(NewTaskTarget{.column = column, .lane = lane});
}

void BoardController::closeNewTask() { _newTask.set(std::nullopt); }

std::string BoardController::newTaskTitle() const {
    auto const& target = _newTask.get();
    auto const& current = _open.value();
    if (target && current) {
        for (ColumnView const& column : current->columns) {
            if (column.id == target->column) {
                return "New task in " + column.name;
            }
        }
    }
    return "New task";
}

morph::async::Completion<std::string> BoardController::submitBoardForm(std::string_view actionType, std::string body,
                                                                      std::function<void()> after) {
    return morph::examples::mapCompletion<std::string>(
        _wiring.callbacks, _lifetime.token(), _handler.executeJson(actionType, body),
        [this, after = std::move(after)](std::string const& reply) {
            after();
            _open.refetch();
            return reply;
        },
        [](std::exception_ptr const&) {});
}

morph::async::Completion<morph::examples::PollPage<BoardEvent, BoardEventId>> BoardController::eventsSince(
    BoardEventId cursor) {
    using Page = morph::examples::PollPage<BoardEvent, BoardEventId>;
    return morph::examples::mapCompletion<Page>(
        _wiring.callbacks, _lifetime.token(), _handler.execute(GetEventsSince{.lastEventId = cursor}),
        [cursor](GetEventsSinceResult const& result) {
            return Page{.events = result.events, .next = result.events.empty() ? cursor : result.events.back().id};
        },
        [](std::exception_ptr const&) {});
}

std::optional<std::int64_t> BoardController::laneOf(std::int64_t taskId) const {
    auto const& current = _open.value();
    if (current) {
        for (TaskView const& task : current->tasks) {
            if (task.id == TaskId{taskId} && task.swimlaneId.hasValue()) {
                return *task.swimlaneId;
            }
        }
    }
    return std::nullopt;
}

void BoardController::followAttach() {
    std::optional<ProjectId> const attached = _attached.get();
    _wiring.runtime.untracked([&] {
        if (!attached) {
            _polling.reset();
            _pollingProject.reset();
            return;
        }
        if (_polling != nullptr && _pollingProject == attached) {
            return;
        }
        _polling.reset();
        _polling = std::make_unique<Polling>(*this);
        _pollingProject = attached;
    });
}

}  // namespace kanban::client
```

`followAttach` keys on `_attached`, an equality-gated `Computed` of the board's project: a refresh delivers a new
board but the same project, so polling is not restarted by every refresh — only by a different project or a close.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client]"
```

Expected: PASS (the board, polling and socket cases included).

Mutation checks, one at a time, each restored afterwards:
- In `followAttach`, delete the `_polling.reset();` of the `!attached` branch. Expected FAIL in "closing the board
  stops polling" (`pendingTimers()` stays non-zero).
- In `Polling::onEvents`, delete `_board->refresh();`. Expected FAIL in "another client's move reaches an open board
  on the next poll".
- In `moveTask`, replace `morph::examples::newUuid()` with `std::string{"op"}`. Expected FAIL in "moveTask moves a
  card, every move with a fresh op id" (the second move replays the first's stored result; the card stays in Done).

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/controllers/board_controller.hpp examples/kanban/app/controllers/board_controller.cpp \
        examples/kanban/tests/client/board_support.hpp examples/kanban/tests/client/test_board_controller.cpp \
        examples/kanban/tests/client/test_board_polling.cpp examples/kanban/tests/client/test_board_socket.cpp
git commit -m "wip(kanban): BoardController: attach, board forms, cards, moves with fresh op ids, polling

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K7: Concurrent drag over `BoardController`

Re-expresses `tests/test_board_concurrent_drag.cpp`: four clients move their own tasks between two columns, every
move fired before any is awaited. The model proves exactly-once and dense positions for raw `MoveTaskPosition`
(`test_kanban_stress.cpp`); this proves the client's path — an op id minted per gesture, one `Mutation` per
controller, invalidation refetches racing each other — keeps both, and that no two of the 48 gestures shared an
op id (the hazard the old file named: a per-call value stashed in a shared field).

**Files:**
- Modify (rewrite): `examples/kanban/tests/test_board_concurrent_drag.cpp`
- Test: the same file

**Interfaces:**
- Consumes: `BoardController::moveTask`, `movesPending`, `lastMoveOpId`, `openNewTask`, `createTaskForm`, `refresh`,
  `loading`, `board` (Task K6); `LocalClient`, `signedContext`, `seedProject`, `submitForm`, `seedBoard` (Tasks K3,
  K6).
- Produces: nothing new.

- [ ] **Step 1: Write the failing test**

Replace the whole of `examples/kanban/tests/test_board_concurrent_drag.cpp` with:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Four board controllers on one bridge move their own tasks concurrently, each move under its own op id, nothing
// awaited until every move is fired. The model's own guarantees (exactly-once per op id, dense positions per
// column) are proven for raw MoveTaskPosition calls in test_kanban_stress.cpp; this proves the client's path keeps
// them — and mints a distinct op id for every gesture, which is what exactly-once needs from a client.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "client/board_support.hpp"
#include "controllers/board_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

[[nodiscard]] bool positionsAreDenseAndUnique(kanban::GetBoardResult const& board) {
    for (kanban::ColumnView const& column : board.columns) {
        std::vector<std::int64_t> positions;
        for (kanban::TaskView const& task : board.tasks) {
            if (task.columnId == column.id) {
                positions.push_back(task.position);
            }
        }
        std::ranges::sort(positions);
        for (std::size_t i = 0; i < positions.size(); ++i) {
            if (positions.at(i) != static_cast<std::int64_t>(i)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::int64_t taskTitled(kanban::GetBoardResult const& board, std::string_view title) {
    for (kanban::TaskView const& task : board.tasks) {
        if (task.title == title && task.id.hasValue()) {
            return *task.id;
        }
    }
    FAIL("the board has no task titled " << title);
    return 0;
}

}  // namespace

TEST_CASE("Concurrent BoardController::moveTask calls (N=4) never desync positions", "[kanban][client][stress]") {
    morph::ladder::testkit::DbFixture const fixture;
    kanban::testing::LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Stress Board");
    constexpr std::size_t kClients = 4;
    constexpr int kMovesPerClient = 12;
    std::vector<std::unique_ptr<kanban::client::BoardController>> boards;
    for (std::size_t i = 0; i < kClients; ++i) {
        boards.push_back(std::make_unique<kanban::client::BoardController>(client.wiring()));
    }
    auto& seeder = *boards.front();
    auto const seeded = kanban::testing::seedBoard(client, seeder, project);
    std::vector<std::int64_t> taskIds{seeded.task};
    for (std::size_t i = 1; i < kClients; ++i) {
        std::int64_t const column = (i % 2 == 0) ? seeded.todo : seeded.done;
        std::string const title = "Task " + std::to_string(i);
        seeder.openNewTask(kanban::ColumnId{column}, kanban::SwimlaneId{seeded.lane});
        kanban::testing::submitForm(client, seeder.createTaskForm(), R"({"title":")" + title + R"("})");
        REQUIRE(client.settle([&] { return seeder.board() && seeder.board()->tasks.size() == i + 1; }));
        taskIds.push_back(taskTitled(*seeder.board(), title));
    }
    for (std::size_t i = 1; i < kClients; ++i) {
        boards.at(i)->open(project, kanban::Role::Manager);
        REQUIRE(client.settle([&] { return boards.at(i)->board() && boards.at(i)->board()->tasks.size() == kClients; }));
    }

    std::vector<std::int64_t> const columns{seeded.todo, seeded.done};
    std::set<std::string> opIds;
    for (std::size_t i = 0; i < kClients; ++i) {
        for (int move = 0; move < kMovesPerClient; ++move) {
            auto const position = (move * 3 + static_cast<int>(i)) % static_cast<int>(kClients);
            boards.at(i)->moveTask(kanban::TaskId{taskIds.at(i)},
                                   kanban::ColumnId{columns.at(static_cast<std::size_t>(move) % columns.size())},
                                   kanban::SwimlaneId{seeded.lane}, position);
            opIds.insert(boards.at(i)->lastMoveOpId());
        }
    }
    REQUIRE(client.settle(
        [&boards] { return std::ranges::none_of(boards, [](auto const& board) { return board->movesPending(); }); },
        std::chrono::milliseconds{20000}));

    CHECK(opIds.size() == kClients * static_cast<std::size_t>(kMovesPerClient));
    seeder.refresh();
    REQUIRE(client.settle([&seeder] { return !seeder.loading(); }, std::chrono::milliseconds{20000}));
    REQUIRE(seeder.board().has_value());
    auto const& finalBoard = *seeder.board();
    CHECK(positionsAreDenseAndUnique(finalBoard));
    REQUIRE(finalBoard.tasks.size() == taskIds.size());
    for (std::int64_t const taskId : taskIds) {
        CHECK(std::ranges::count_if(finalBoard.tasks, [taskId](kanban::TaskView const& task) {
                  return task.id == kanban::TaskId{taskId};
              }) == 1);
    }
}
```

- [ ] **Step 2: Run it to verify it fails**

Before Task K6 this file did not compile against `BoardController`; with Task K6 in place, run the mutation first:
in `BoardController::moveTask` replace `morph::examples::newUuid()` with `std::string{"op"}`, then

```bash
cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "Concurrent BoardController::moveTask calls (N=4) never desync positions"
```

Expected: FAIL at `CHECK(opIds.size() == …)` (one op id for 48 gestures). Restore `newUuid()`.

- [ ] **Step 3: Implement**

No production change: the test pins Task K6's behaviour under concurrency.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client][stress]"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/tests/test_board_concurrent_drag.cpp
git commit -m "wip(kanban): concurrent drag re-expressed over BoardController

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task K8: The board's offline queue, end to end

Re-expresses `tests/test_board_offline_bridge.cpp` over `BoardController` with `BoardOptions::offline`: a move
made while the probe says "down" is queued and not sent, and replays when it says "up"; a move the server keeps
refusing (a full WIP column) is dead-lettered after its retry budget; and the reconnect path emits the
framework's offline metrics.

**Files:**
- Create: `examples/kanban/tests/client/test_board_offline.cpp`
- Test: the same file

**Interfaces:**
- Consumes: `BoardOptions{offline}`, `OfflineConfig`, `BoardController::online`, `queueDepth`, `deadLettered`,
  `syncRuns`, `deadLetterText`, `moveTask`, `movesPending`, `cardsIn` (Tasks K5, K6); `ScopedQueueFile` (Task K5);
  `morph::observe::ScopedObserveOverride`, `setMetricSink`, `Metric` (`morph/core/observability.hpp`).
- Produces: nothing new.

- [ ] **Step 1: Write the failing test**

Create `examples/kanban/tests/client/test_board_offline.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#ifdef MORPH_BUILD_OFFLINE_SQLITE

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <morph/core/observability.hpp>
#include <mutex>
#include <vector>

#include "board_support.hpp"
#include "controllers/board_controller.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using namespace std::chrono_literals;
using kanban::ColumnId;
using kanban::SwimlaneId;
using kanban::TaskId;
using kanban::client::BoardController;
using kanban::client::BoardOptions;
using kanban::client::OfflineConfig;
using kanban::testing::LocalClient;

[[nodiscard]] BoardOptions offlineOptions(std::filesystem::path const& queuePath, std::atomic<bool>& reachable) {
    return BoardOptions{.offline = OfflineConfig{.queuePath = queuePath,
                                                 .probe = [&reachable] { return reachable.load(); },
                                                 .probeInterval = 20ms,
                                                 .failureThreshold = 1,
                                                 .onlineThreshold = 1}};
}

}  // namespace

TEST_CASE("kanban::client::BoardController: a move made while offline is queued, not sent, and replays on reconnect",
          "[kanban][client][offline]") {
    morph::ladder::testkit::DbFixture const fixture;
    kanban::testing::ScopedQueueFile const queueFile{"board_offline_replay"};
    std::atomic<bool> reachable{true};
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Offline Board");
    BoardController board{client.wiring(), offlineOptions(queueFile.path(), reachable)};
    auto const seeded = kanban::testing::seedBoard(client, board, project);

    board.moveTask(TaskId{seeded.task}, ColumnId{seeded.done}, SwimlaneId{seeded.lane}, 0);
    REQUIRE(client.settle([&] { return board.cardsIn(seeded.done, seeded.lane).size() == 1 && !board.movesPending(); }));

    reachable.store(false);
    REQUIRE(client.settle([&board] { return !board.online(); }, 2000ms));
    board.moveTask(TaskId{seeded.task}, ColumnId{seeded.todo}, SwimlaneId{seeded.lane}, 0);
    REQUIRE(client.settle([&board] { return board.queueDepth() == 1; }, 2000ms));
    CHECK_FALSE(board.movesPending());
    CHECK(board.cardsIn(seeded.done, seeded.lane).size() == 1);
    CHECK(board.syncText() == "1 changes pending sync");
    CHECK(board.deadLettered() == 0);

    reachable.store(true);
    REQUIRE(client.settle(
        [&] { return board.queueDepth() == 0 && board.cardsIn(seeded.todo, seeded.lane).size() == 1; }, 5000ms));
    CHECK(board.syncText().empty());
}

TEST_CASE("kanban::client::BoardController: a queued move the server keeps refusing is dead-lettered",
          "[kanban][client][offline]") {
    morph::ladder::testkit::DbFixture const fixture;
    kanban::testing::ScopedQueueFile const queueFile{"board_offline_dead_letter"};
    std::atomic<bool> reachable{true};
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Offline Board");
    BoardController board{client.wiring(), offlineOptions(queueFile.path(), reachable)};
    auto const seeded = kanban::testing::seedBoard(client, board, project, /*doneWipLimit=*/1);
    board.openNewTask(ColumnId{seeded.done}, SwimlaneId{seeded.lane});
    kanban::testing::submitForm(client, board.createTaskForm(), R"({"title":"Blocker"})");
    REQUIRE(client.settle([&] { return board.cardsIn(seeded.done, seeded.lane).size() == 1; }));

    reachable.store(false);
    REQUIRE(client.settle([&board] { return !board.online(); }, 2000ms));
    board.moveTask(TaskId{seeded.task}, ColumnId{seeded.done}, SwimlaneId{seeded.lane}, 0);
    REQUIRE(client.settle([&board] { return board.queueDepth() == 1; }, 2000ms));

    for (int flap = 0; flap < 8 && board.deadLettered() == 0; ++flap) {
        int const runs = board.syncRuns();
        reachable.store(true);
        REQUIRE(client.settle([&] { return board.syncRuns() > runs; }, 5000ms));
        reachable.store(false);
        REQUIRE(client.settle([&board] { return !board.online(); }, 2000ms));
    }

    REQUIRE(client.settle([&board] { return board.deadLettered() == 1 && board.queueDepth() == 0; }, 2000ms));
    CHECK(board.deadLetterText() == "1 changes could not be synced");
    CHECK(board.cardsIn(seeded.todo, seeded.lane).size() == 1);
}

TEST_CASE("kanban::client::BoardController: the offline path emits the framework's offline metrics",
          "[kanban][client][offline]") {
    morph::ladder::testkit::DbFixture const fixture;
    kanban::testing::ScopedQueueFile const queueFile{"board_offline_metrics"};
    std::atomic<bool> reachable{true};
    morph::observe::ScopedObserveOverride const observeOverride;
    std::vector<morph::observe::Metric> observed;
    std::mutex observedMutex;
    morph::observe::setMetricSink([&](morph::observe::MetricEvent const& event) {
        std::scoped_lock const lock{observedMutex};
        observed.push_back(event.metric);
    });
    auto const seen = [&](morph::observe::Metric metric) {
        std::scoped_lock const lock{observedMutex};
        return std::ranges::find(observed, metric) != observed.end();
    };
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Offline Board");
    BoardController board{client.wiring(), offlineOptions(queueFile.path(), reachable)};
    auto const seeded = kanban::testing::seedBoard(client, board, project);

    reachable.store(false);
    REQUIRE(client.settle([&board] { return !board.online(); }, 2000ms));
    board.moveTask(TaskId{seeded.task}, ColumnId{seeded.done}, SwimlaneId{seeded.lane}, 0);
    REQUIRE(client.settle([&board] { return board.queueDepth() == 1; }, 2000ms));
    reachable.store(true);
    REQUIRE(client.settle([&] { return board.queueDepth() == 0 && board.cardsIn(seeded.done, seeded.lane).size() == 1; },
                          5000ms));

    CHECK(seen(morph::observe::Metric::queueDepth));
    CHECK(seen(morph::observe::Metric::reconnectAttempts));
    CHECK(seen(morph::observe::Metric::reconnectOutcome));
}

#endif  // MORPH_BUILD_OFFLINE_SQLITE
```

- [ ] **Step 2: Run it to verify it fails**

Apply the mutation first (Tasks K5/K6 already implement the behaviour): in `BoardController::moveTask` delete the
`#ifdef MORPH_BUILD_OFFLINE_SQLITE` branch that enqueues while offline. Then:

```bash
cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client][offline]"
```

Expected: FAIL in "a move made while offline is queued, not sent, and replays on reconnect" — `queueDepth() == 1`
never holds, because the move went straight out. Restore the branch.

- [ ] **Step 3: Implement**

No production change.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client][offline]"
```

Expected: PASS (6 cases: Task K5's three and these three).

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/tests/client/test_board_offline.cpp
git commit -m "wip(kanban): the board's offline queue end to end over BoardController

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K9: `RulesController` — the rules dialog

Re-expresses `BoardBridge::createRule`/`getRules`/`deleteRule`/`fetchOptions` and `RulesView.qml`: the rules are a
`Query<GetRules>` keyed on the dialog being shown and the attached project, on the board's own handler (rules belong
to the attached instance); the `CreateRule` form's `projectId` is assigned as context; its
`triggerColumnId` Choice fetches its options (`GetBoardState`) through `forms::handlerChoiceFetcher` over that same
handler, so an options action the board model does not serve is refused by name; Remove is a `Mutation`
invalidating the list.

**Files:**
- Create: `examples/kanban/app/controllers/rules_controller.hpp`, `examples/kanban/app/controllers/rules_controller.cpp`
- Test: `examples/kanban/tests/client/test_rules_controller.cpp`

**Interfaces:**
- Consumes: `BoardController::handler()`, `attachedProject()`, `board()` (Task K6); `ruleLine` (Task K2);
  `morph::examples::mapCompletion` (Part 6); `forms::FormModel::forAction<A>()`, `ChoiceFetcher`,
  `handlerChoiceFetcher` (Part 5, `<morph/forms/engine/handler_submitter.hpp>`).
- Produces: `kanban::client::RuleRow{id, text}`; `RulesController(examples::Wiring, BoardController&)`: `show()`, `hide()`,
  `shown()`, `rules()`, `remove(RuleId)`, `createRuleForm()`, `errorText()`,
  `fetchOptions(std::string_view optionsAction, std::string body) -> Completion<std::string>`.

- [ ] **Step 1: Write the failing test**

Create `examples/kanban/tests/client/test_rules_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>

#include "board_support.hpp"
#include "controllers/board_controller.hpp"
#include "controllers/rules_controller.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"

using kanban::client::BoardController;
using kanban::client::RulesController;
using kanban::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

TEST_CASE("kanban::client::RulesController: a rule created through the form lists by column name; Remove deletes it",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);
    RulesController rules{client.wiring(), board};
    CHECK_FALSE(rules.shown());
    CHECK(rules.rules().empty());

    rules.show();
    kanban::testing::submitForm(client, rules.createRuleForm(),
                                R"({"triggerColumnId":)" + std::to_string(seeded.done) +
                                    R"(,"mutationType":"AddTag","mutationValue":"shipped"})");

    REQUIRE(client.settle([&rules] { return rules.rules().size() == 1; }));
    CHECK(rules.rules().front().text == R"(when moved to "Done": AddTag "shipped")");

    rules.hide();
    REQUIRE(client.settle([&rules] { return rules.rules().empty(); }));
    rules.show();
    REQUIRE(client.settle([&rules] { return rules.rules().size() == 1; }));

    rules.remove(kanban::RuleId{rules.rules().front().id});
    REQUIRE(client.settle([&rules] { return rules.rules().empty(); }));
    CHECK(rules.errorText().empty());
}

TEST_CASE("kanban::client::RulesController: the trigger column's options come from the attached board's handler",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);
    RulesController rules{client.wiring(), board};

    std::string const options = kanban::testing::await(client, rules.fetchOptions("GetBoardState", "{}"));
    CHECK(options.find(std::to_string(seeded.todo)) != std::string::npos);
    CHECK(options.find(std::to_string(seeded.done)) != std::string::npos);

    // A project-admin action: the board's handler does not serve it, so the fetcher refuses it by name.
    CHECK_THROWS_WITH(kanban::testing::await(client, rules.fetchOptions("GetMyProjects", "{}")),
                      Catch::Matchers::ContainsSubstring("GetMyProjects"));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'controllers/rules_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/controllers/rules_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/handler_submitter.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/wiring.hpp"
#include "controllers/board_controller.hpp"

namespace kanban::client {

/// @brief One automation rule in the rules dialog.
struct RuleRow {
    /// @brief The rule's id.
    std::int64_t id = 0;
    /// @brief Its display line.
    std::string text;
    /// @brief Compared by value.
    bool operator==(RuleRow const&) const = default;
};

/// @brief The rules dialog: the attached board's automation rules, the `CreateRule` form and Remove.
class RulesController {
public:
    /// @param wiring The runtime and owner this controller uses.
    /// @param board The board whose attached instance owns the rules. Borrowed: it must outlive this controller.
    RulesController(morph::examples::Wiring wiring, BoardController& board);

    /// @brief Opens the dialog; the rules are fetched and the form's project is filled in.
    void show();
    /// @brief Closes the dialog; the rules go idle.
    void hide();
    /// @brief Whether the dialog is open. Tracked.
    /// @return True between `show` and `hide`.
    [[nodiscard]] bool shown() const { return _shown.get(); }
    /// @brief The rules, in creation order. Tracked.
    /// @return One row per rule; empty while hidden.
    [[nodiscard]] std::vector<RuleRow> rules() const;
    /// @brief Deletes a rule; the list refreshes when it lands.
    /// @param rule The rule.
    void remove(RuleId rule);
    /// @brief The `CreateRule` form.
    /// @return The form session.
    [[nodiscard]] morph::forms::FormSession& createRuleForm() noexcept { return _createRule; }
    /// @brief The first failure of the list or a removal. Tracked.
    /// @return The message, or empty.
    [[nodiscard]] std::string errorText() const;
    /// @brief The `CreateRule` form's `ChoiceFetcher`: runs the options action on the attached board's handler, and
    ///        refuses by name an action that handler does not serve.
    /// @param optionsAction The options action the schema names.
    /// @param body Its JSON body.
    /// @return The options reply, owned by the wiring's owner.
    [[nodiscard]] morph::async::Completion<std::string> fetchOptions(std::string_view optionsAction, std::string body) {
        return _boardOptions(optionsAction, std::move(body));
    }

private:
    [[nodiscard]] morph::async::Completion<std::string> submitRule(std::string_view actionType, std::string body);
    void assignProject();

    morph::examples::Wiring _wiring;
    BoardController* _board;
    morph::reactive::Signal<bool> _shown;
    morph::reactive::Query<GetRules> _rules;
    morph::reactive::Mutation<DeleteRule> _delete;
    morph::forms::ChoiceFetcher _boardOptions;
    morph::forms::FormSession _createRule;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace kanban::client
```

Create `examples/kanban/app/controllers/rules_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/rules_controller.hpp"

#include <exception>
#include <morph/forms/engine/field_model.hpp>
#include <optional>
#include <utility>

#include "app/completion_map.hpp"
#include "support/board_format.hpp"

namespace kanban::client {

RulesController::RulesController(morph::examples::Wiring wiring, BoardController& board)
    : _wiring{wiring},
      _board{&board},
      _shown{wiring.runtime, false},
      _rules{wiring.runtime, board.handler(),
             [this]() -> std::optional<GetRules> {
                 if (!_shown.get()) {
                     return std::nullopt;
                 }
                 std::optional<ProjectId> const project = _board->attachedProject();
                 if (!project) {
                     return std::nullopt;
                 }
                 return GetRules{.projectId = *project};
             }},
      _delete{wiring.runtime, board.handler(), morph::reactive::MutationOptions{.invalidates = {&_rules}}},
      _boardOptions{morph::forms::handlerChoiceFetcher(wiring.callbacks, board.handler())},
      _createRule{wiring.runtime, morph::forms::FormModel::forAction<CreateRule>(),
                  [this](std::string_view actionType, std::string body) {
                      return submitRule(actionType, std::move(body));
                  },
                  _boardOptions} {}

void RulesController::show() {
    _shown.set(true);
    _createRule.reset();
    assignProject();
}

void RulesController::hide() { _shown.set(false); }

std::vector<RuleRow> RulesController::rules() const {
    std::vector<RuleRow> rows;
    auto const& listed = _rules.value();
    if (!listed) {
        return rows;
    }
    auto const& board = _board->board();
    std::vector<ColumnView> const noColumns;
    std::vector<ColumnView> const& columns = board ? board->columns : noColumns;
    for (RuleView const& rule : listed->rules) {
        if (rule.id.hasValue()) {
            rows.push_back(RuleRow{.id = *rule.id, .text = ruleLine(rule, columns)});
        }
    }
    return rows;
}

void RulesController::remove(RuleId rule) { _delete.run(DeleteRule{.ruleId = rule}); }

std::string RulesController::errorText() const {
    for (std::exception_ptr const& error : {_rules.error(), _delete.error()}) {
        if (error != nullptr) {
            return morph::reactive::errorMessage(error);
        }
    }
    return {};
}

morph::async::Completion<std::string> RulesController::submitRule(std::string_view actionType, std::string body) {
    return morph::examples::mapCompletion<std::string>(
        _wiring.callbacks, _lifetime.token(), _board->handler().executeJson(actionType, body),
        [this](std::string const& reply) {
            _createRule.reset();
            assignProject();
            _rules.refetch();
            return reply;
        },
        [](std::exception_ptr const&) {});
}

void RulesController::assignProject() {
    std::optional<ProjectId> const project = _board->attachedProject();
    if (project && project->hasValue()) {
        _createRule.assign("projectId", std::to_string(**project));
    }
}

}  // namespace kanban::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client]"
```

Expected: PASS.

Mutation check: in `submitRule`'s success hook delete `_rules.refetch();`. Expected FAIL in "a rule created through
the form lists by column name…" (the list never gains the rule). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/controllers/rules_controller.hpp examples/kanban/app/controllers/rules_controller.cpp \
        examples/kanban/tests/client/test_rules_controller.cpp
git commit -m "wip(kanban): RulesController: the rules dialog on the attached board

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K10: `IAttachmentTransfer` and the HTTP/1.1 client over POSIX sockets

Attachments move bytes over the HTTP side channel and metadata through actions (`kanban/dto/attachment_dto.hpp`).
The bytes part becomes an injected interface: upload a local file, download a stored blob to a local file. Listing is
metadata — `GetAttachments` through the bridge (Task K11) — because the side channel has no list route
(`src/server/include/kanban/http/attachment_server.hpp`: exactly `POST /attachments` and `GET /attachments/{storageKey}`). The TUI's
implementation is a minimal blocking HTTP/1.1 client on a worker thread, POSIX sockets only; on Windows it reports
itself unavailable and every request fails with the reason, so the view disables attachments there. The Qt Quick
implementation is Task K13's.

**Files:**
- Create: `examples/kanban/app/attachments/attachment_transfer.hpp`, `examples/kanban/app/attachments/attachment_transfer.cpp`
- Create: `examples/kanban/app/http/http_client.hpp`, `examples/kanban/app/http/http_client.cpp`
- Create: `examples/kanban/app/http/http_attachment_transfer.hpp`, `examples/kanban/app/http/http_attachment_transfer.cpp`
- Create: `examples/kanban/tests/client/qt_client_support.hpp`
- Test: `examples/kanban/tests/client/test_http_client.cpp`, `examples/kanban/tests/client/test_http_attachment_transfer.cpp`

**Interfaces:**
- Consumes: `Completion<T>::makeSettleable`, `Promise::resolve/reject` (any thread); `exec::ThreadPoolExecutor`;
  `kanban::http::AttachmentServer(TokenVerifier const&, Config)`, `listen()`, `port()` (Task K1);
  `morph::ladder::testkit::pumpUntil`, `awaitQt` (`testkit/pump.hpp`).
- Produces:
  - `kanban::client::UploadedBlob{storageKey, filename, contentType, sizeBytes}`, `UploadRequest{localPath,
    bearerToken}`, `DownloadRequest{storageKey, localPath, bearerToken}`; `IAttachmentTransfer` with `available()`,
    `upload(UploadRequest) -> Completion<UploadedBlob>`, `download(DownloadRequest) -> Completion<std::string>` (the
    written path); `contentTypeFor(path)`, `localPathFrom(std::string_view)`,
    `attachmentServerFor(std::string_view serverUrl) -> std::optional<std::string>`.
  - `kanban::client::http::{Endpoint, Header, Request, Response, HttpError, parseUrl, parseResponse, formatRequest,
    socketsAvailable, send}`.
  - `kanban::client::HttpAttachmentTransfer(std::string const& baseUrl, exec::IExecutor& callbacks)`.
  - Test support: `kanban::testing::QtClient` (as `LocalClient`, over a `QtExecutor`, settling with the Qt pump),
    `seedProject(QtClient&, name)`, `freshStorageDir(name)`.

- [ ] **Step 1: Write the failing tests**

Create `examples/kanban/tests/client/qt_client_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>

#include "app/wiring.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "kanban_client_support.hpp"
#include "testkit/pump.hpp"

namespace kanban::testing {

/// One client whose owner is the Qt event loop: for tests whose subject also needs that loop (the attachment
/// server). Declare it before the controllers under test.
class QtClient {
public:
    explicit QtClient(morph::session::Context session) { bridge.setDefaultSession(std::move(session)); }
    ~QtClient() = default;
    QtClient(QtClient const&) = delete;
    QtClient& operator=(QtClient const&) = delete;
    QtClient(QtClient&&) = delete;
    QtClient& operator=(QtClient&&) = delete;

    /// Pumps the Qt event loop until @p done holds or the budget runs out.
    template <typename Pred>
    [[nodiscard]] bool settle(Pred done, std::chrono::milliseconds budget = 5000ms) {
        return morph::ladder::testkit::pumpUntil(std::move(done), budget);
    }

    /// What a controller under test is built from.
    [[nodiscard]] morph::examples::Wiring wiring() {
        return morph::examples::Wiring{
            .runtime = runtime, .scheduler = scheduler, .bridge = bridge, .callbacks = owner};
    }

    morph::exec::ThreadPoolExecutor pool{4};
    morph::qt::QtExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::reactive::Runtime runtime{owner};
    morph::reactive::testing::ManualScheduler scheduler;
};

/// Creates a project as the client's principal.
[[nodiscard]] inline kanban::ProjectId seedProject(QtClient& client, std::string name) {
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> admin{client.bridge, &client.owner};
    return await(client, admin.execute(kanban::CreateProject{.name = std::move(name)})).id;
}

/// An empty attachment storage directory under the temp directory.
[[nodiscard]] inline std::filesystem::path freshStorageDir(std::string const& name) {
    auto path = std::filesystem::temp_directory_path() / ("kanban_client_attachments_" + name);
    std::filesystem::remove_all(path);
    return path;
}

}  // namespace kanban::testing
```

Create `examples/kanban/tests/client/test_http_client.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <optional>
#include <string>

#include "attachments/attachment_transfer.hpp"
#include "http/http_client.hpp"

namespace http = kanban::client::http;

TEST_CASE("kanban::client::http::parseUrl reads http URLs and refuses everything else", "[kanban][client][http]") {
    auto const local = http::parseUrl("http://127.0.0.1:8769");
    REQUIRE(local.has_value());
    CHECK(local->host == "127.0.0.1");
    CHECK(local->port == "8769");
    auto const defaulted = http::parseUrl("http://example.org/ignored/path");
    REQUIRE(defaulted.has_value());
    CHECK(defaulted->host == "example.org");
    CHECK(defaulted->port == "80");
    CHECK_FALSE(http::parseUrl("https://example.org").has_value());
    CHECK_FALSE(http::parseUrl("http://").has_value());
    CHECK_FALSE(http::parseUrl("http://host:notaport").has_value());
}

TEST_CASE("kanban::client::http::parseResponse reads a complete response and refuses a short or garbled one",
          "[kanban][client][http]") {
    std::string const raw =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\ncontent-length: 18\r\nConnection: close\r\n\r\n"
        R"({"storageKey":"k"})";
    auto const response = http::parseResponse(raw);
    REQUIRE(response.has_value());
    CHECK(response->status == 200);
    CHECK(response->body == R"({"storageKey":"k"})");
    CHECK_FALSE(http::parseResponse(raw.substr(0, raw.size() - 1)).has_value());
    CHECK_FALSE(http::parseResponse("not http at all\r\n\r\n").has_value());
    auto const notFound = http::parseResponse("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(notFound.has_value());
    CHECK(notFound->status == 404);
    CHECK(notFound->body.empty());
}

TEST_CASE("kanban::client::http::formatRequest writes the request line, Host, length, headers and body",
          "[kanban][client][http]") {
    std::string const wire = http::formatRequest(
        http::Endpoint{.host = "127.0.0.1", .port = "8769"},
        http::Request{.method = "POST",
                      .target = "/attachments",
                      .headers = {http::Header{.name = "Authorization", .value = "Bearer t"}},
                      .body = "bytes"});
    CHECK(wire.starts_with("POST /attachments HTTP/1.1\r\n"));
    CHECK(wire.find("\r\nHost: 127.0.0.1:8769\r\n") != std::string::npos);
    CHECK(wire.find("\r\nContent-Length: 5\r\n") != std::string::npos);
    CHECK(wire.find("\r\nAuthorization: Bearer t\r\n") != std::string::npos);
    CHECK(wire.ends_with("\r\n\r\nbytes"));
}

TEST_CASE("kanban::client::contentTypeFor and localPathFrom", "[kanban][client][http]") {
    CHECK(kanban::client::contentTypeFor("notes.TXT") == "text/plain");
    CHECK(kanban::client::contentTypeFor("report.pdf") == "application/pdf");
    CHECK(kanban::client::contentTypeFor("blob") == "application/octet-stream");
    CHECK(kanban::client::localPathFrom("/tmp/a.txt") == std::filesystem::path{"/tmp/a.txt"});
    CHECK(kanban::client::localPathFrom("file:///tmp/a.txt") == std::filesystem::path{"/tmp/a.txt"});
    CHECK(kanban::client::localPathFrom("file:///C:/data/a.txt") == std::filesystem::path{"C:/data/a.txt"});
}

TEST_CASE("kanban::client::attachmentServerFor names the side channel on the server's host", "[kanban][client][http]") {
    CHECK(kanban::client::attachmentServerFor("ws://10.0.0.5:8765") ==
          std::optional<std::string>{"http://10.0.0.5:8769"});
    CHECK(kanban::client::attachmentServerFor("wss://example.org/board") ==
          std::optional<std::string>{"http://example.org:8769"});
    CHECK_FALSE(kanban::client::attachmentServerFor("not a url").has_value());
    CHECK_FALSE(kanban::client::attachmentServerFor("ws://:8765").has_value());
}
```

Create `examples/kanban/tests/client/test_http_attachment_transfer.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <morph/qt/qt_executor.hpp>
#include <morph/session/session_auth.hpp>
#include <string>

#include "http/http_attachment_transfer.hpp"
#include "kanban/http/attachment_server.hpp"
#include "kanban_client_support.hpp"
#include "qt_client_support.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using kanban::client::DownloadRequest;
using kanban::client::HttpAttachmentTransfer;
using kanban::client::UploadRequest;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::DbFixture;

namespace {

[[nodiscard]] std::filesystem::path writeFile(std::filesystem::path const& dir, std::string const& name,
                                              std::string const& bytes) {
    std::filesystem::create_directories(dir);
    auto const path = dir / name;
    std::ofstream{path, std::ios::binary} << bytes;
    return path;
}

}  // namespace

TEST_CASE("kanban::client::HttpAttachmentTransfer uploads a file to the side channel", "[kanban][client][http]") {
    DbFixture const fixture;
    morph::session::TokenVerifier const verifier{std::string{kanban::testing::kSecret}, morph::session::hmacSha256};
    auto const storage = kanban::testing::freshStorageDir("http_upload");
    kanban::http::AttachmentServer server{verifier, kanban::http::AttachmentServer::Config{.storageDir = storage}};
    REQUIRE(server.listen());
    morph::qt::QtExecutor owner;
    HttpAttachmentTransfer transfer{"http://127.0.0.1:" + std::to_string(server.port()), owner};
    REQUIRE(transfer.available());
    auto const source = writeFile(storage.parent_path() / "kanban_client_upload_src", "report.txt", "attachment bytes");

    auto const blob = awaitQt(
        transfer.upload(UploadRequest{.localPath = source, .bearerToken = kanban::testing::signedContext("alice").token}));

    CHECK(blob.filename == "report.txt");
    CHECK(blob.contentType == "text/plain");
    CHECK(blob.sizeBytes == 16);
    CHECK(blob.storageKey.size() == 64);
    CHECK(std::filesystem::exists(storage / blob.storageKey));
    std::filesystem::remove_all(storage);
}

TEST_CASE("kanban::client::HttpAttachmentTransfer: the side channel's refusals come back as failures",
          "[kanban][client][http]") {
    DbFixture const fixture;
    morph::session::TokenVerifier const verifier{std::string{kanban::testing::kSecret}, morph::session::hmacSha256};
    auto const storage = kanban::testing::freshStorageDir("http_refusals");
    kanban::http::AttachmentServer server{verifier, kanban::http::AttachmentServer::Config{.storageDir = storage}};
    REQUIRE(server.listen());
    morph::qt::QtExecutor owner;
    HttpAttachmentTransfer transfer{"http://127.0.0.1:" + std::to_string(server.port()), owner};
    auto const source = writeFile(storage.parent_path() / "kanban_client_refusal_src", "a.txt", "x");
    auto const target = storage.parent_path() / "kanban_client_refusal_out.txt";
    std::filesystem::remove(target);

    CHECK_THROWS_WITH(awaitQt(transfer.upload(UploadRequest{.localPath = source, .bearerToken = "forged"})),
                      Catch::Matchers::ContainsSubstring("HTTP 401"));
    CHECK_THROWS_WITH(awaitQt(transfer.download(DownloadRequest{.storageKey = std::string(64, 'a'),
                                                                .localPath = target,
                                                                .bearerToken =
                                                                    kanban::testing::signedContext("alice").token})),
                      Catch::Matchers::ContainsSubstring("HTTP 404"));
    CHECK_FALSE(std::filesystem::exists(target));
    CHECK_THROWS_WITH(awaitQt(transfer.download(DownloadRequest{.storageKey = "../../etc/passwd",
                                                                .localPath = target,
                                                                .bearerToken = "t"})),
                      Catch::Matchers::ContainsSubstring("storage key"));
    std::filesystem::remove_all(storage);
}

TEST_CASE("kanban::client::HttpAttachmentTransfer without a usable URL is unavailable and says why",
          "[kanban][client][http]") {
    morph::qt::QtExecutor owner;
    HttpAttachmentTransfer transfer{"not a url", owner};
    CHECK_FALSE(transfer.available());
    CHECK_THROWS_WITH(awaitQt(transfer.upload(UploadRequest{.localPath = "missing.txt", .bearerToken = "t"})),
                      Catch::Matchers::ContainsSubstring("no attachment server"));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'attachments/attachment_transfer.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/attachments/attachment_transfer.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <morph/core/completion.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace kanban::client {

/// @brief What the side channel stored for an upload: the key `AddAttachment` commits, and the file's metadata.
struct UploadedBlob {
    /// @brief The opaque key the server minted.
    std::string storageKey;
    /// @brief The uploaded file's name, without its directory.
    std::string filename;
    /// @brief The content type sent with it.
    std::string contentType;
    /// @brief Its size in bytes.
    std::int64_t sizeBytes = 0;
};

/// @brief One upload.
struct UploadRequest {
    /// @brief The local file to send.
    std::filesystem::path localPath;
    /// @brief The caller's session token, sent as `Authorization: Bearer`.
    std::string bearerToken;
};

/// @brief One download.
struct DownloadRequest {
    /// @brief The blob's storage key.
    std::string storageKey;
    /// @brief Where to write it.
    std::filesystem::path localPath;
    /// @brief The caller's session token.
    std::string bearerToken;
};

/// @brief The client side of the attachment side channel: bytes over HTTP, never through the bridge.
///
/// Completions are owned by the executor the implementation was given, so a controller attaches to them on its
/// own owner.
class IAttachmentTransfer {
public:
    IAttachmentTransfer() = default;
    virtual ~IAttachmentTransfer() = default;
    IAttachmentTransfer(IAttachmentTransfer const&) = delete;
    IAttachmentTransfer& operator=(IAttachmentTransfer const&) = delete;
    IAttachmentTransfer(IAttachmentTransfer&&) = delete;
    IAttachmentTransfer& operator=(IAttachmentTransfer&&) = delete;

    /// @brief Whether this client can reach a side channel at all.
    /// @return False without a server URL, or on a platform this implementation has no transport for.
    [[nodiscard]] virtual bool available() const noexcept = 0;

    /// @brief Sends a local file.
    /// @param request The file and the caller's token.
    /// @return The stored blob's key and metadata, or the failure.
    [[nodiscard]] virtual morph::async::Completion<UploadedBlob> upload(UploadRequest request) = 0;

    /// @brief Fetches a stored blob into a local file.
    /// @param request The key, the target file and the caller's token.
    /// @return The written path, or the failure (a key the caller may not read fails like an unknown one).
    [[nodiscard]] virtual morph::async::Completion<std::string> download(DownloadRequest request) = 0;
};

/// @brief The content type sent for a file, from its extension.
/// @param path The file.
/// @return A media type; `application/octet-stream` for an extension not in the table.
[[nodiscard]] std::string contentTypeFor(std::filesystem::path const& path);

/// @brief A local path from either a plain path or a `file://` URL (a file picker may hand back either).
/// @param pathOrFileUrl The path or URL.
/// @return The path.
[[nodiscard]] std::filesystem::path localPathFrom(std::string_view pathOrFileUrl);

/// @brief The attachment side channel of a server: the server opens it on its own host, on port 8769.
/// @param serverUrl The server's WebSocket URL (`--server`), e.g. `ws://host:8765`.
/// @return `http://<host>:8769`, or empty when @p serverUrl names no host.
[[nodiscard]] std::optional<std::string> attachmentServerFor(std::string_view serverUrl);

}  // namespace kanban::client
```

Create `examples/kanban/app/attachments/attachment_transfer.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "attachments/attachment_transfer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kanban::client {

std::string contentTypeFor(std::filesystem::path const& path) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kTypes{{
        {".txt", "text/plain"},
        {".md", "text/markdown"},
        {".csv", "text/csv"},
        {".json", "application/json"},
        {".pdf", "application/pdf"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
    }};
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    for (auto const& [suffix, type] : kTypes) {
        if (extension == suffix) {
            return std::string{type};
        }
    }
    return "application/octet-stream";
}

std::filesystem::path localPathFrom(std::string_view pathOrFileUrl) {
    constexpr std::string_view kScheme = "file://";
    if (!pathOrFileUrl.starts_with(kScheme)) {
        return std::filesystem::path{pathOrFileUrl};
    }
    std::string_view rest = pathOrFileUrl.substr(kScheme.size());
    // `file:///C:/x` names the Windows path `C:/x`; `file:///home/x` the POSIX path `/home/x`.
    if (rest.size() >= 3 && rest.front() == '/' && std::isalpha(static_cast<unsigned char>(rest.at(1))) != 0 &&
        rest.at(2) == ':') {
        rest.remove_prefix(1);
    }
    return std::filesystem::path{rest};
}

std::optional<std::string> attachmentServerFor(std::string_view serverUrl) {
    // The port the server's side channel listens on beside its WebSocket port (src/server/main.cpp).
    constexpr std::string_view kSideChannelPort = "8769";
    auto const scheme = serverUrl.find("://");
    if (scheme == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view authority = serverUrl.substr(scheme + 3);
    authority = authority.substr(0, authority.find('/'));
    std::string_view const host = authority.substr(0, authority.rfind(':'));
    if (host.empty()) {
        return std::nullopt;
    }
    return "http://" + std::string{host} + ":" + std::string{kSideChannelPort};
}

}  // namespace kanban::client
```

Create `examples/kanban/app/http/http_client.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kanban::client::http {

/// @brief Where a request goes.
struct Endpoint {
    /// @brief Host name or address.
    std::string host;
    /// @brief Port, as text (what `getaddrinfo` takes).
    std::string port;
};

/// @brief One header line.
struct Header {
    /// @brief Its name.
    std::string name;
    /// @brief Its value.
    std::string value;
};

/// @brief One request. `Host`, `Content-Length` and `Connection: close` are added by `formatRequest`.
struct Request {
    /// @brief `GET` or `POST`.
    std::string method;
    /// @brief The request target, e.g. `/attachments`.
    std::string target;
    /// @brief Extra headers.
    std::vector<Header> headers;
    /// @brief The body; may be empty.
    std::string body;
    /// @brief Bound on each socket read and write.
    std::chrono::milliseconds timeout{10000};
};

/// @brief One response.
struct Response {
    /// @brief The status code.
    int status = 0;
    /// @brief The header lines, names as sent.
    std::vector<Header> headers;
    /// @brief The body.
    std::string body;
};

/// @brief A request that did not produce a response: resolution, connection, I/O or parse failure.
class HttpError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Reads `http://host[:port][/path]`; the path is ignored.
/// @param url The URL.
/// @return The endpoint (port 80 by default), or empty for anything else.
[[nodiscard]] std::optional<Endpoint> parseUrl(std::string_view url);

/// @brief Reads a complete HTTP/1.x response.
/// @param raw Everything the server sent.
/// @return The response, or empty when it is malformed or shorter than its `Content-Length`.
[[nodiscard]] std::optional<Response> parseResponse(std::string_view raw);

/// @brief Writes @p request as it goes on the wire to @p endpoint.
/// @param endpoint The server, for the `Host` header.
/// @param request The request.
/// @return The bytes to send.
[[nodiscard]] std::string formatRequest(Endpoint const& endpoint, Request const& request);

/// @brief Whether `send` has a transport on this platform.
/// @return True on POSIX; false on Windows.
[[nodiscard]] bool socketsAvailable() noexcept;

/// @brief Sends @p request and reads the whole response; blocks the calling thread.
/// @param endpoint The server.
/// @param request The request.
/// @return The response, whatever its status.
/// @throws HttpError when no response could be read.
[[nodiscard]] Response send(Endpoint const& endpoint, Request const& request);

}  // namespace kanban::client::http
```

Create `examples/kanban/app/http/http_client.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "http/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <memory>
#endif

namespace kanban::client::http {

namespace {

[[nodiscard]] bool equalsIgnoringCase(std::string_view lhs, std::string_view rhs) {
    return std::ranges::equal(lhs, rhs, [](unsigned char left, unsigned char right) {
        return std::tolower(left) == std::tolower(right);
    });
}

[[nodiscard]] bool isPort(std::string_view text) {
    unsigned port = 0;
    auto const [end, failure] = std::from_chars(text.data(), text.data() + text.size(), port);
    return failure == std::errc{} && end == text.data() + text.size() && port > 0 && port <= 65535;
}

}  // namespace

std::optional<Endpoint> parseUrl(std::string_view url) {
    constexpr std::string_view kScheme = "http://";
    if (!url.starts_with(kScheme)) {
        return std::nullopt;
    }
    std::string_view authority = url.substr(kScheme.size());
    authority = authority.substr(0, authority.find('/'));
    if (authority.empty()) {
        return std::nullopt;
    }
    auto const colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
        return Endpoint{.host = std::string{authority}, .port = "80"};
    }
    std::string_view const port = authority.substr(colon + 1);
    if (colon == 0 || !isPort(port)) {
        return std::nullopt;
    }
    return Endpoint{.host = std::string{authority.substr(0, colon)}, .port = std::string{port}};
}

std::optional<Response> parseResponse(std::string_view raw) {
    auto const headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos || !raw.starts_with("HTTP/1.")) {
        return std::nullopt;
    }
    std::string_view const head = raw.substr(0, headerEnd);
    auto const statusLineEnd = head.find("\r\n");
    std::string_view const statusLine = head.substr(0, statusLineEnd);
    auto const firstSpace = statusLine.find(' ');
    if (firstSpace == std::string_view::npos || statusLine.size() < firstSpace + 4) {
        return std::nullopt;
    }
    Response response;
    std::string_view const code = statusLine.substr(firstSpace + 1, 3);
    if (auto const [end, failure] = std::from_chars(code.data(), code.data() + code.size(), response.status);
        failure != std::errc{} || end != code.data() + code.size()) {
        return std::nullopt;
    }
    std::optional<std::size_t> contentLength;
    std::string_view rest = statusLineEnd == std::string_view::npos ? std::string_view{} : head.substr(statusLineEnd + 2);
    while (!rest.empty()) {
        auto const lineEnd = rest.find("\r\n");
        std::string_view const line = rest.substr(0, lineEnd);
        rest = lineEnd == std::string_view::npos ? std::string_view{} : rest.substr(lineEnd + 2);
        auto const colon = line.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        Header header{.name = std::string{line.substr(0, colon)}, .value = std::string{value}};
        if (equalsIgnoringCase(header.name, "Content-Length")) {
            std::size_t length = 0;
            if (auto const [end, failure] = std::from_chars(value.data(), value.data() + value.size(), length);
                failure != std::errc{} || end != value.data() + value.size()) {
                return std::nullopt;
            }
            contentLength = length;
        }
        response.headers.push_back(std::move(header));
    }
    std::string_view const body = raw.substr(headerEnd + 4);
    if (contentLength) {
        if (body.size() < *contentLength) {
            return std::nullopt;
        }
        response.body = std::string{body.substr(0, *contentLength)};
    } else {
        response.body = std::string{body};
    }
    return response;
}

std::string formatRequest(Endpoint const& endpoint, Request const& request) {
    std::string wire = request.method + " " + request.target + " HTTP/1.1\r\n";
    wire += "Host: " + endpoint.host + ":" + endpoint.port + "\r\n";
    wire += "Connection: close\r\n";
    wire += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
    for (Header const& header : request.headers) {
        wire += header.name + ": " + header.value + "\r\n";
    }
    wire += "\r\n";
    wire += request.body;
    return wire;
}

#if defined(_WIN32)

bool socketsAvailable() noexcept { return false; }

Response send(Endpoint const& /*endpoint*/, Request const& /*request*/) {
    throw HttpError{"attachments need POSIX sockets, which the terminal client does not have on Windows"};
}

#else

namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Owns one socket descriptor.
class Socket {
public:
    explicit Socket(int descriptor) noexcept : _descriptor{descriptor} {}
    ~Socket() {
        if (_descriptor >= 0) {
            ::close(_descriptor);
        }
    }
    Socket(Socket&& other) noexcept : _descriptor{std::exchange(other._descriptor, -1)} {}
    Socket& operator=(Socket&&) = delete;
    Socket(Socket const&) = delete;
    Socket& operator=(Socket const&) = delete;

    [[nodiscard]] int descriptor() const noexcept { return _descriptor; }

private:
    int _descriptor;
};

[[nodiscard]] std::string lastError() { return std::system_category().message(errno); }

void bound(int descriptor, std::chrono::milliseconds timeout) {
    timeval const limit{.tv_sec = static_cast<decltype(timeval::tv_sec)>(timeout.count() / 1000),
                        .tv_usec = static_cast<decltype(timeval::tv_usec)>((timeout.count() % 1000) * 1000)};
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
#if defined(SO_NOSIGPIPE)
    int const enable = 1;
    ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof enable);
#endif
}

[[nodiscard]] Socket connectTo(Endpoint const& endpoint, std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (int const status = ::getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &found);
        status != 0) {
        throw HttpError{"cannot resolve " + endpoint.host + ": " + ::gai_strerror(status)};
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> const addresses{found, &::freeaddrinfo};
    for (addrinfo const* address = addresses.get(); address != nullptr; address = address->ai_next) {
        Socket candidate{::socket(address->ai_family, address->ai_socktype, address->ai_protocol)};
        if (candidate.descriptor() < 0) {
            continue;
        }
        bound(candidate.descriptor(), timeout);
        if (::connect(candidate.descriptor(), address->ai_addr, address->ai_addrlen) == 0) {
            return candidate;
        }
    }
    throw HttpError{"cannot connect to " + endpoint.host + ":" + endpoint.port + ": " + lastError()};
}

void writeAll(Socket const& socket, std::string_view bytes) {
    while (!bytes.empty()) {
        ssize_t const sent = ::send(socket.descriptor(), bytes.data(), bytes.size(), kSendFlags);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw HttpError{"send failed: " + lastError()};
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}

[[nodiscard]] std::string readAll(Socket const& socket) {
    std::string raw;
    std::array<char, 8192> chunk{};
    for (;;) {
        ssize_t const received = ::recv(socket.descriptor(), chunk.data(), chunk.size(), 0);
        if (received == 0) {
            return raw;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw HttpError{"receive failed: " + lastError()};
        }
        raw.append(chunk.data(), static_cast<std::size_t>(received));
    }
}

}  // namespace

bool socketsAvailable() noexcept { return true; }

Response send(Endpoint const& endpoint, Request const& request) {
    Socket const socket = connectTo(endpoint, request.timeout);
    writeAll(socket, formatRequest(endpoint, request));
    auto response = parseResponse(readAll(socket));
    if (!response) {
        throw HttpError{"malformed HTTP response from " + endpoint.host};
    }
    return std::move(*response);
}

#endif

}  // namespace kanban::client::http
```

The server answers each request and closes the connection (`Connection: close`, `src/server/app/attachment_server.cpp`'s
`respondAndClose`), so reading until end of stream reads exactly one response.

Create `examples/kanban/app/http/http_attachment_transfer.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <optional>
#include <string>

#include "attachments/attachment_transfer.hpp"
#include "http/http_client.hpp"

namespace kanban::client {

/// @brief `IAttachmentTransfer` over the blocking HTTP client, one request at a time on a private worker thread.
///
/// Available on POSIX only; elsewhere every request fails with the reason.
class HttpAttachmentTransfer final : public IAttachmentTransfer {
public:
    /// @param baseUrl The side channel, e.g. `http://127.0.0.1:8769`.
    /// @param callbacks Owns the returned completions: the caller's own executor. Borrowed.
    HttpAttachmentTransfer(std::string const& baseUrl, morph::exec::IExecutor& callbacks);
    /// @brief Waits for a request in flight (each is bounded by its socket timeout).
    ~HttpAttachmentTransfer() override;
    HttpAttachmentTransfer(HttpAttachmentTransfer const&) = delete;
    HttpAttachmentTransfer& operator=(HttpAttachmentTransfer const&) = delete;
    HttpAttachmentTransfer(HttpAttachmentTransfer&&) = delete;
    HttpAttachmentTransfer& operator=(HttpAttachmentTransfer&&) = delete;

    /// @brief Whether the URL read and this platform has sockets.
    /// @return True when requests can be sent.
    [[nodiscard]] bool available() const noexcept override;
    /// @brief Posts the file; see `IAttachmentTransfer::upload`.
    /// @param request The file and the caller's token.
    /// @return The stored blob, or the failure.
    [[nodiscard]] morph::async::Completion<UploadedBlob> upload(UploadRequest request) override;
    /// @brief Fetches the blob; see `IAttachmentTransfer::download`.
    /// @param request The key, target file and token.
    /// @return The written path, or the failure.
    [[nodiscard]] morph::async::Completion<std::string> download(DownloadRequest request) override;

private:
    std::optional<http::Endpoint> _endpoint;
    morph::exec::IExecutor* _callbacks;
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _io;
};

}  // namespace kanban::client
```

Create `examples/kanban/app/http/http_attachment_transfer.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "http/http_attachment_transfer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <fstream>
#include <glaze/glaze.hpp>
#include <ios>
#include <iterator>
#include <utility>

namespace kanban::client {

namespace {

constexpr std::chrono::milliseconds kTimeout{10000};

// The upload reply: `{"storageKey":"..."}`.
struct StorageKeyReply {
    std::string storageKey;
};

// A storage key goes into the request line, so it is held to the shape the server mints (hex) plus the
// characters a key could safely carry; anything else is refused before a byte is sent.
[[nodiscard]] bool isPlausibleStorageKey(std::string_view key) {
    return !key.empty() && key.size() <= 255 && std::ranges::all_of(key, [](unsigned char letter) {
        return std::isalnum(letter) != 0 || letter == '-' || letter == '_';
    });
}

[[nodiscard]] UploadedBlob uploadNow(std::optional<http::Endpoint> const& endpoint, UploadRequest const& request) {
    if (!endpoint) {
        throw http::HttpError{"no attachment server is configured"};
    }
    std::ifstream file{request.localPath, std::ios::binary};
    if (!file) {
        throw http::HttpError{"could not open '" + request.localPath.string() + "' for reading"};
    }
    std::string const bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    std::string const contentType = contentTypeFor(request.localPath);
    http::Response const response =
        http::send(*endpoint, http::Request{.method = "POST",
                                            .target = "/attachments",
                                            .headers = {http::Header{.name = "X-Attachment-Content-Type",
                                                                     .value = contentType},
                                                        http::Header{.name = "Authorization",
                                                                     .value = "Bearer " + request.bearerToken}},
                                            .body = bytes,
                                            .timeout = kTimeout});
    if (response.status != 200) {
        throw http::HttpError{"upload failed: HTTP " + std::to_string(response.status)};
    }
    StorageKeyReply reply;
    if (glz::read_json(reply, response.body) || reply.storageKey.empty()) {
        throw http::HttpError{"upload failed: the server's reply carried no storage key"};
    }
    return UploadedBlob{.storageKey = reply.storageKey,
                        .filename = request.localPath.filename().string(),
                        .contentType = contentType,
                        .sizeBytes = static_cast<std::int64_t>(bytes.size())};
}

[[nodiscard]] std::string downloadNow(std::optional<http::Endpoint> const& endpoint, DownloadRequest const& request) {
    if (!endpoint) {
        throw http::HttpError{"no attachment server is configured"};
    }
    if (!isPlausibleStorageKey(request.storageKey)) {
        throw http::HttpError{"download refused: '" + request.storageKey + "' is not a storage key"};
    }
    http::Response const response =
        http::send(*endpoint, http::Request{.method = "GET",
                                            .target = "/attachments/" + request.storageKey,
                                            .headers = {http::Header{.name = "Authorization",
                                                                     .value = "Bearer " + request.bearerToken}},
                                            .body = {},
                                            .timeout = kTimeout});
    if (response.status != 200) {
        throw http::HttpError{"download failed: HTTP " + std::to_string(response.status)};
    }
    std::ofstream file{request.localPath, std::ios::binary | std::ios::trunc};
    file.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
    if (!file) {
        throw http::HttpError{"could not write '" + request.localPath.string() + "'"};
    }
    return request.localPath.string();
}

}  // namespace

HttpAttachmentTransfer::HttpAttachmentTransfer(std::string const& baseUrl, morph::exec::IExecutor& callbacks)
    : _endpoint{http::parseUrl(baseUrl)},
      _callbacks{&callbacks},
      _io{std::make_unique<morph::exec::ThreadPoolExecutor>(1)} {}

HttpAttachmentTransfer::~HttpAttachmentTransfer() = default;

bool HttpAttachmentTransfer::available() const noexcept { return _endpoint.has_value() && http::socketsAvailable(); }

morph::async::Completion<UploadedBlob> HttpAttachmentTransfer::upload(UploadRequest request) {
    auto settleable = morph::async::Completion<UploadedBlob>::makeSettleable(_callbacks);
    auto promise = std::make_shared<morph::async::Completion<UploadedBlob>::Promise>(std::move(settleable.second));
    _io->post([endpoint = _endpoint, request = std::move(request), promise] {
        try {
            promise->resolve(uploadNow(endpoint, request));
        } catch (...) {
            promise->reject(std::current_exception());
        }
    });
    return std::move(settleable.first);
}

morph::async::Completion<std::string> HttpAttachmentTransfer::download(DownloadRequest request) {
    auto settleable = morph::async::Completion<std::string>::makeSettleable(_callbacks);
    auto promise = std::make_shared<morph::async::Completion<std::string>::Promise>(std::move(settleable.second));
    _io->post([endpoint = _endpoint, request = std::move(request), promise] {
        try {
            promise->resolve(downloadNow(endpoint, request));
        } catch (...) {
            promise->reject(std::current_exception());
        }
    });
    return std::move(settleable.first);
}

}  // namespace kanban::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client][http]"
```

Expected: PASS, 8 cases.

Mutation check: in `parseResponse`, delete the `if (body.size() < *contentLength) { return std::nullopt; }` block.
Expected FAIL in "parseResponse reads a complete response and refuses a short or garbled one". Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/attachments examples/kanban/app/http examples/kanban/tests/client/qt_client_support.hpp \
        examples/kanban/tests/client/test_http_client.cpp examples/kanban/tests/client/test_http_attachment_transfer.cpp
git commit -m "wip(kanban): IAttachmentTransfer and an HTTP/1.1 client over POSIX sockets

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K11: `TaskDetailController` — comments and attachments

Re-expresses `TaskDetailPopup.qml` with `BoardBridge::addComment`, `getAttachments`, `uploadAttachment`,
`downloadAttachment`: the open task's comments are a projection of the board; the `AddComment` form's `taskId` is
context; attachments are a `Query<GetAttachments>` keyed on the open task; an upload goes through the injected
`IAttachmentTransfer` and then commits its metadata with a `Mutation<AddAttachment>` invalidating the list; a
download writes through the transfer. Without a transfer (local mode, or the Windows terminal) attachments are
unavailable and the controller says so instead of failing.

**Files:**
- Create: `examples/kanban/app/controllers/task_detail_controller.hpp`, `examples/kanban/app/controllers/task_detail_controller.cpp`
- Create: `examples/kanban/tests/client/fake_transfer.hpp`
- Test: `examples/kanban/tests/client/test_task_detail_controller.cpp`,
  `examples/kanban/tests/client/test_task_detail_attachments.cpp`

**Interfaces:**
- Consumes: `BoardController::handler()`, `board()`, `refresh()` (Task K6); `IAttachmentTransfer`,
  `UploadRequest`, `DownloadRequest`, `UploadedBlob`, `localPathFrom`, `HttpAttachmentTransfer` (Task K10);
  `attachmentLine` (Task K2); `morph::examples::mapCompletion` (Part 6); `forms::FormModel::forAction<A>()`,
  `FormSession::assign` (Part 5); `QtClient`, `freshStorageDir` (Task K10).
- Produces: `kanban::client::CommentRow{index, principal, body}`, `AttachmentRow{id, storageKey, line}`;
  `TaskDetailController(examples::Wiring, BoardController&, std::shared_ptr<IAttachmentTransfer>)`: `openTask(TaskId)`,
  `close()`, `isOpen()`, `title()`, `comments()`, `addCommentForm()`, `attachments()`, `attachmentsAvailable()`,
  `upload(std::string const& localPathOrUrl)`, `download(std::int64_t attachmentId, std::string const&
  localPathOrUrl)`, `transferPending()`, `statusText()`.
  - Test support: `kanban::testing::FakeTransfer` (an `IAttachmentTransfer` whose requests wait for the test).

- [ ] **Step 1: Write the failing tests**

Create `examples/kanban/tests/client/fake_transfer.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <string>
#include <utility>
#include <vector>

#include "attachments/attachment_transfer.hpp"

namespace kanban::testing {

/// An attachment transfer whose every request waits until the test settles it.
class FakeTransfer final : public kanban::client::IAttachmentTransfer {
public:
    explicit FakeTransfer(morph::exec::IExecutor& owner) : _owner{&owner} {}

    [[nodiscard]] bool available() const noexcept override { return true; }

    [[nodiscard]] morph::async::Completion<kanban::client::UploadedBlob> upload(
        kanban::client::UploadRequest request) override {
        uploads.push_back(std::move(request));
        auto settleable = morph::async::Completion<kanban::client::UploadedBlob>::makeSettleable(_owner);
        uploadReplies.push_back(std::move(settleable.second));
        return std::move(settleable.first);
    }

    [[nodiscard]] morph::async::Completion<std::string> download(kanban::client::DownloadRequest request) override {
        downloads.push_back(std::move(request));
        auto settleable = morph::async::Completion<std::string>::makeSettleable(_owner);
        downloadReplies.push_back(std::move(settleable.second));
        return std::move(settleable.first);
    }

    std::vector<kanban::client::UploadRequest> uploads;
    std::vector<morph::async::Completion<kanban::client::UploadedBlob>::Promise> uploadReplies;
    std::vector<kanban::client::DownloadRequest> downloads;
    std::vector<morph::async::Completion<std::string>::Promise> downloadReplies;

private:
    morph::exec::IExecutor* _owner;
};

}  // namespace kanban::testing
```

Create `examples/kanban/tests/client/test_task_detail_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <filesystem>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "attachments/attachment_transfer.hpp"
#include "board_support.hpp"
#include "controllers/board_controller.hpp"
#include "controllers/task_detail_controller.hpp"
#include "fake_transfer.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using kanban::TaskId;
using kanban::client::BoardController;
using kanban::client::TaskDetailController;
using kanban::client::UploadedBlob;
using kanban::testing::FakeTransfer;
using kanban::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

[[nodiscard]] UploadedBlob reportBlob() {
    return UploadedBlob{
        .storageKey = "abc123", .filename = "report.pdf", .contentType = "application/pdf", .sizeBytes = 34};
}

}  // namespace

TEST_CASE("kanban::client::TaskDetailController: the open task's comments, and the form appends one",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);
    TaskDetailController detail{client.wiring(), board, nullptr};
    CHECK_FALSE(detail.isOpen());

    detail.openTask(TaskId{seeded.task});

    CHECK(detail.isOpen());
    CHECK(detail.title() == "Fix bug");
    CHECK(detail.comments().empty());
    kanban::testing::submitForm(client, detail.addCommentForm(), R"({"body":"looks good"})");
    REQUIRE(client.settle([&detail] { return detail.comments().size() == 1; }));
    CHECK(detail.comments().front().principal == "alice");
    CHECK(detail.comments().front().body == "looks good");
    detail.close();
    CHECK_FALSE(detail.isOpen());
}

TEST_CASE("kanban::client::TaskDetailController: an upload goes through the transfer, then records its metadata",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);
    auto const transfer = std::make_shared<FakeTransfer>(client.owner);
    TaskDetailController detail{client.wiring(), board, transfer};
    detail.openTask(TaskId{seeded.task});
    CHECK(detail.attachmentsAvailable());

    detail.upload("file:///tmp/report.pdf");

    REQUIRE(transfer->uploads.size() == 1);
    CHECK(transfer->uploads.front().localPath == std::filesystem::path{"/tmp/report.pdf"});
    CHECK(transfer->uploads.front().bearerToken == client.bridge.defaultSession().token);
    CHECK(detail.transferPending());
    transfer->uploadReplies.front().resolve(reportBlob());
    REQUIRE(client.settle([&detail] { return detail.attachments().size() == 1 && !detail.transferPending(); }));
    CHECK(detail.attachments().front().line == "report.pdf  (34 bytes, alice)");
    CHECK(detail.attachments().front().storageKey == "abc123");
    CHECK(detail.statusText() == "attachment uploaded");

    detail.download(detail.attachments().front().id, "/tmp/report-copy.pdf");
    REQUIRE(transfer->downloads.size() == 1);
    CHECK(transfer->downloads.front().storageKey == "abc123");
    transfer->downloadReplies.front().resolve("/tmp/report-copy.pdf");
    REQUIRE(client.settle([&detail] { return detail.statusText() == "saved to /tmp/report-copy.pdf"; }));
}

TEST_CASE("kanban::client::TaskDetailController: a failed upload reports and records nothing", "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);
    auto const transfer = std::make_shared<FakeTransfer>(client.owner);
    TaskDetailController detail{client.wiring(), board, transfer};
    detail.openTask(TaskId{seeded.task});

    detail.upload("/tmp/report.pdf");
    transfer->uploadReplies.front().reject(std::make_exception_ptr(std::runtime_error{"disk full"}));

    REQUIRE(client.settle([&detail] { return !detail.transferPending(); }));
    CHECK(detail.statusText() == "upload failed: disk full");
    CHECK(detail.attachments().empty());
}

TEST_CASE("kanban::client::TaskDetailController: without a transfer, attachments are unavailable and say so",
          "[kanban][client]") {
    DbFixture const fixture;
    LocalClient client{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(client, "Sprint Board");
    BoardController board{client.wiring()};
    auto const seeded = kanban::testing::seedBoard(client, board, project);
    TaskDetailController detail{client.wiring(), board, nullptr};
    detail.openTask(TaskId{seeded.task});

    CHECK_FALSE(detail.attachmentsAvailable());
    detail.upload("/tmp/report.pdf");

    CHECK(detail.statusText() == "attachments are not available on this client");
    CHECK_FALSE(detail.transferPending());
}
```

Create `examples/kanban/tests/client/test_task_detail_attachments.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The task dialog over the real side channel: a Qt-hosted AttachmentServer, the POSIX HTTP transfer, and the
// metadata committed through the bridge.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <string>

#include "board_support.hpp"
#include "controllers/board_controller.hpp"
#include "controllers/task_detail_controller.hpp"
#include "http/http_attachment_transfer.hpp"
#include "kanban/http/attachment_server.hpp"
#include "qt_client_support.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using kanban::TaskId;
using kanban::client::BoardController;
using kanban::client::HttpAttachmentTransfer;
using kanban::client::TaskDetailController;
using kanban::testing::QtClient;

[[nodiscard]] std::string readFile(std::filesystem::path const& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST_CASE("kanban::client::TaskDetailController uploads over the side channel and downloads the bytes back",
          "[kanban][client][http]") {
    morph::ladder::testkit::DbFixture const fixture;
    morph::session::TokenVerifier const verifier{std::string{kanban::testing::kSecret}, morph::session::hmacSha256};
    auto const storage = kanban::testing::freshStorageDir("detail_round_trip");
    kanban::http::AttachmentServer server{verifier, kanban::http::AttachmentServer::Config{.storageDir = storage}};
    REQUIRE(server.listen());
    std::string const url = "http://127.0.0.1:" + std::to_string(server.port());
    QtClient alice{kanban::testing::signedContext("alice")};
    auto const project = kanban::testing::seedProject(alice, "Sprint Board");
    BoardController board{alice.wiring()};
    auto const seeded = kanban::testing::seedBoard(alice, board, project);
    TaskDetailController detail{alice.wiring(), board, std::make_shared<HttpAttachmentTransfer>(url, alice.owner)};
    detail.openTask(TaskId{seeded.task});
    auto const workDir = std::filesystem::temp_directory_path() / "kanban_client_detail_round_trip";
    std::filesystem::remove_all(workDir);
    std::filesystem::create_directories(workDir);
    std::ofstream{workDir / "report.txt", std::ios::binary} << "the attachment's own bytes";

    detail.upload((workDir / "report.txt").string());
    REQUIRE(alice.settle([&detail] { return detail.attachments().size() == 1 && !detail.transferPending(); }));
    CHECK(detail.statusText() == "attachment uploaded");

    auto const copy = workDir / "report-downloaded.txt";
    detail.download(detail.attachments().front().id, copy.string());
    REQUIRE(alice.settle([&detail] { return !detail.transferPending() && !detail.statusText().empty(); }));
    CHECK(detail.statusText() == "saved to " + copy.string());
    CHECK(readFile(copy) == "the attachment's own bytes");

    // Authenticated is not authorized: another principal, with no role on alice's project, cannot read the blob.
    QtClient mallory{kanban::testing::signedContext("mallory")};
    HttpAttachmentTransfer malloryTransfer{url, mallory.owner};
    auto const stolen = workDir / "mallory.txt";
    CHECK_THROWS_WITH(
        morph::ladder::testkit::awaitQt(malloryTransfer.download(
            kanban::client::DownloadRequest{.storageKey = detail.attachments().front().storageKey,
                                            .localPath = stolen,
                                            .bearerToken = kanban::testing::signedContext("mallory").token})),
        Catch::Matchers::ContainsSubstring("HTTP 404"));
    CHECK_FALSE(std::filesystem::exists(stolen));

    std::filesystem::remove_all(workDir);
    std::filesystem::remove_all(storage);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'controllers/task_detail_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/controllers/task_detail_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/wiring.hpp"
#include "attachments/attachment_transfer.hpp"
#include "controllers/board_controller.hpp"

namespace kanban::client {

/// @brief One comment in the task dialog.
struct CommentRow {
    /// @brief Its place among the task's comments; its key.
    std::int64_t index = 0;
    /// @brief Who wrote it.
    std::string principal;
    /// @brief What they wrote.
    std::string body;
    /// @brief Compared by value.
    bool operator==(CommentRow const&) const = default;
};

/// @brief One attachment in the task dialog.
struct AttachmentRow {
    /// @brief The attachment's id.
    std::int64_t id = 0;
    /// @brief The key its bytes are stored under.
    std::string storageKey;
    /// @brief Its display line.
    std::string line;
    /// @brief Compared by value.
    bool operator==(AttachmentRow const&) const = default;
};

/// @brief The task dialog: the open task's comments, the `AddComment` form, and its attachments.
class TaskDetailController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    /// @param board The board whose attached instance owns the task. Borrowed: it must outlive this controller.
    /// @param transfer The side channel's client; null disables attachments.
    TaskDetailController(morph::examples::Wiring wiring, BoardController& board,
                         std::shared_ptr<IAttachmentTransfer> transfer);
    ~TaskDetailController();
    TaskDetailController(TaskDetailController const&) = delete;
    TaskDetailController& operator=(TaskDetailController const&) = delete;
    TaskDetailController(TaskDetailController&&) = delete;
    TaskDetailController& operator=(TaskDetailController&&) = delete;

    /// @brief Opens the dialog on @p task.
    /// @param task The task.
    void openTask(TaskId task);
    /// @brief Closes the dialog.
    void close();
    /// @brief Whether the dialog is open. Tracked.
    /// @return True between `openTask` and `close`.
    [[nodiscard]] bool isOpen() const { return _task.get().has_value(); }
    /// @brief The dialog's title. Tracked.
    /// @return The task's title, or `"Task"`.
    [[nodiscard]] std::string title() const;
    /// @brief The open task's comments, oldest first. Tracked.
    /// @return One row per comment.
    [[nodiscard]] std::vector<CommentRow> comments() const;
    /// @brief The `AddComment` form; its `taskId` is the open task.
    /// @return The form session.
    [[nodiscard]] morph::forms::FormSession& addCommentForm() noexcept { return _addComment; }
    /// @brief The open task's attachments, in upload order. Tracked.
    /// @return One row per attachment.
    [[nodiscard]] std::vector<AttachmentRow> attachments() const;
    /// @brief Whether this client can move attachment bytes at all.
    /// @return False without a transfer, or with one that has no transport here.
    [[nodiscard]] bool attachmentsAvailable() const noexcept;
    /// @brief Uploads a local file and attaches it to the open task.
    /// @param localPathOrUrl A path, or a `file://` URL.
    void upload(std::string const& localPathOrUrl);
    /// @brief Downloads one of the open task's attachments to a local file.
    /// @param attachmentId The attachment.
    /// @param localPathOrUrl Where to write it: a path, or a `file://` URL.
    void download(std::int64_t attachmentId, std::string const& localPathOrUrl);
    /// @brief Whether an upload, its commit or a download is in flight. Tracked.
    /// @return True while any is.
    [[nodiscard]] bool transferPending() const;
    /// @brief The last transfer's outcome. Tracked.
    /// @return E.g. `"attachment uploaded"`, `"saved to <path>"`, a failure, or empty.
    [[nodiscard]] std::string statusText() const;

private:
    [[nodiscard]] morph::async::Completion<std::string> submitComment(std::string_view actionType, std::string body);
    void assignTask();
    void finishTransfer();

    morph::examples::Wiring _wiring;
    BoardController* _board;
    std::shared_ptr<IAttachmentTransfer> _transfer;
    morph::reactive::Signal<std::optional<TaskId>> _task;
    morph::reactive::Signal<std::string> _status;
    morph::reactive::Signal<int> _transfers;
    morph::reactive::Query<GetAttachments> _attachments;
    morph::reactive::Mutation<AddAttachment> _add;
    morph::forms::FormSession _addComment;
    std::unique_ptr<morph::reactive::Effect> _afterAdd;
    // Last member, so it is destroyed first: a transfer or reply still in flight finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace kanban::client
```

Create `examples/kanban/app/controllers/task_detail_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/task_detail_controller.hpp"

#include <exception>
#include <morph/forms/engine/field_model.hpp>
#include <utility>

#include "app/completion_map.hpp"
#include "support/board_format.hpp"

namespace kanban::client {

TaskDetailController::TaskDetailController(morph::examples::Wiring wiring, BoardController& board,
                                           std::shared_ptr<IAttachmentTransfer> transfer)
    : _wiring{wiring},
      _board{&board},
      _transfer{std::move(transfer)},
      _task{wiring.runtime, std::nullopt},
      _status{wiring.runtime, std::string{}},
      _transfers{wiring.runtime, 0},
      _attachments{wiring.runtime, board.handler(),
                   [this]() -> std::optional<GetAttachments> {
                       auto const& task = _task.get();
                       if (!task) {
                           return std::nullopt;
                       }
                       return GetAttachments{.taskId = *task};
                   }},
      _add{wiring.runtime, board.handler(), morph::reactive::MutationOptions{.invalidates = {&_attachments}}},
      _addComment{wiring.runtime, morph::forms::FormModel::forAction<AddComment>(),
                  [this](std::string_view actionType, std::string body) {
                      return submitComment(actionType, std::move(body));
                  },
                  {}} {
    _afterAdd = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this] {
        if (_add.lastResult().has_value()) {
            _wiring.runtime.untracked([this] { _status.set("attachment uploaded"); });
        }
    });
}

TaskDetailController::~TaskDetailController() = default;

void TaskDetailController::openTask(TaskId task) {
    _wiring.runtime.batch([&] {
        _status.set(std::string{});
        _task.set(task);
    });
    _addComment.reset();
    assignTask();
}

void TaskDetailController::close() { _task.set(std::nullopt); }

std::string TaskDetailController::title() const {
    auto const& task = _task.get();
    auto const& board = _board->board();
    if (task && board) {
        for (TaskView const& candidate : board->tasks) {
            if (candidate.id == *task) {
                return candidate.title;
            }
        }
    }
    return "Task";
}

std::vector<CommentRow> TaskDetailController::comments() const {
    std::vector<CommentRow> rows;
    auto const& task = _task.get();
    auto const& board = _board->board();
    if (!task || !board) {
        return rows;
    }
    for (CommentView const& comment : board->comments) {
        if (comment.taskId == *task) {
            rows.push_back(CommentRow{.index = static_cast<std::int64_t>(rows.size()),
                                      .principal = comment.principal,
                                      .body = comment.body});
        }
    }
    return rows;
}

std::vector<AttachmentRow> TaskDetailController::attachments() const {
    std::vector<AttachmentRow> rows;
    auto const& listed = _attachments.value();
    if (!listed) {
        return rows;
    }
    for (AttachmentView const& attachment : listed->attachments) {
        if (attachment.id.hasValue()) {
            rows.push_back(AttachmentRow{
                .id = *attachment.id, .storageKey = attachment.storageKey, .line = attachmentLine(attachment)});
        }
    }
    return rows;
}

bool TaskDetailController::attachmentsAvailable() const noexcept {
    return _transfer != nullptr && _transfer->available();
}

void TaskDetailController::upload(std::string const& localPathOrUrl) {
    std::optional<TaskId> const task = _task.peek();
    if (!task) {
        return;
    }
    if (!attachmentsAvailable()) {
        _status.set("attachments are not available on this client");
        return;
    }
    _status.set(std::string{});
    _transfers.set(_transfers.peek() + 1);
    _transfer
        ->upload(UploadRequest{.localPath = localPathFrom(localPathOrUrl),
                               .bearerToken = _wiring.bridge.defaultSession().token})
        .then(_lifetime,
              [this, taskId = *task](UploadedBlob const& blob) {
                  finishTransfer();
                  _add.run(AddAttachment{.taskId = taskId,
                                         .filename = blob.filename,
                                         .contentType = blob.contentType,
                                         .sizeBytes = blob.sizeBytes,
                                         .storageKey = blob.storageKey});
              })
        .onError(_lifetime, [this](std::exception_ptr const& error) {
            finishTransfer();
            _status.set("upload failed: " + morph::reactive::errorMessage(error));
        });
}

void TaskDetailController::download(std::int64_t attachmentId, std::string const& localPathOrUrl) {
    if (!attachmentsAvailable()) {
        _status.set("attachments are not available on this client");
        return;
    }
    std::string storageKey;
    for (AttachmentRow const& row : attachments()) {
        if (row.id == attachmentId) {
            storageKey = row.storageKey;
        }
    }
    if (storageKey.empty()) {
        return;
    }
    _status.set(std::string{});
    _transfers.set(_transfers.peek() + 1);
    _transfer
        ->download(DownloadRequest{.storageKey = std::move(storageKey),
                                   .localPath = localPathFrom(localPathOrUrl),
                                   .bearerToken = _wiring.bridge.defaultSession().token})
        .then(_lifetime,
              [this](std::string const& saved) {
                  finishTransfer();
                  _status.set("saved to " + saved);
              })
        .onError(_lifetime, [this](std::exception_ptr const& error) {
            finishTransfer();
            _status.set("download failed: " + morph::reactive::errorMessage(error));
        });
}

bool TaskDetailController::transferPending() const { return _transfers.get() > 0 || _add.pending(); }

std::string TaskDetailController::statusText() const {
    if (std::exception_ptr const error = _add.error(); error != nullptr) {
        return "attaching failed: " + morph::reactive::errorMessage(error);
    }
    return _status.get();
}

morph::async::Completion<std::string> TaskDetailController::submitComment(std::string_view actionType,
                                                                          std::string body) {
    return morph::examples::mapCompletion<std::string>(
        _wiring.callbacks, _lifetime.token(), _board->handler().executeJson(actionType, body),
        [this](std::string const& reply) {
            _addComment.reset();
            assignTask();
            _board->refresh();
            return reply;
        },
        [](std::exception_ptr const&) {});
}

void TaskDetailController::assignTask() {
    std::optional<TaskId> const task = _task.peek();
    if (task && task->hasValue()) {
        _addComment.assign("taskId", std::to_string(**task));
    }
}

void TaskDetailController::finishTransfer() { _transfers.set(_transfers.peek() - 1); }

}  // namespace kanban::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client]"
```

Expected: PASS.

Mutation checks, each restored afterwards:
- In `openTask`, delete `assignTask();`. Expected FAIL in "the open task's comments, and the form appends one"
  (`REQUIRE(form.ready())` fails: the hidden `taskId` is never filled).
- In `upload`'s success handler, delete the `_add.run(...)` call. Expected FAIL in "an upload goes through the
  transfer, then records its metadata".

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/controllers/task_detail_controller.hpp \
        examples/kanban/app/controllers/task_detail_controller.cpp examples/kanban/tests/client/fake_transfer.hpp \
        examples/kanban/tests/client/test_task_detail_controller.cpp \
        examples/kanban/tests/client/test_task_detail_attachments.cpp
git commit -m "wip(kanban): TaskDetailController: comments and attachments through an injected transfer

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K12: `AppController` and the views — the board as a grid with drag-and-drop

The five controllers come together in `AppController`, whose route is a `Computed` (signed out → sign-in; a board
open → board; else projects) — Main.qml's `StackView` pushes, as state. The views are bindings only. The board is a
`Grid` of swimlanes × columns (spec 4 §5): a `forEach` keyed on the board's *shape* rebuilds the grid only when a
column or lane is added; inside, each cell is a `forEach` over that cell's cards, so a move updates cards in place.
A card is a `Button` whose `dragKey` is its task id; it is also a drop target (insert before it), and the cell is a
drop target (append). Every drop target's `accepts` takes only positive integer keys.

**Files:**
- Create: `examples/kanban/app/app_controller.hpp`, `examples/kanban/app/app_controller.cpp`
- Create: `examples/kanban/app/views/sign_in_view.{hpp,cpp}`, `projects_view.{hpp,cpp}`, `board_view.{hpp,cpp}`,
  `root_view.{hpp,cpp}` (all under `examples/kanban/app/views/`)
- Test: `examples/kanban/tests/client/test_app_controller.cpp`, `examples/kanban/tests/client/test_kanban_views.cpp`

**Interfaces:**
- Consumes: every controller of Tasks K3–K11; `ui::column`, `row`, `grid`, `GridCell`, `text`, `button`, `select`,
  `SelectOption`, `panel`, `scroll`, `spacer`, `dialog`, `filePicker`, `forEach<RowT>`, `switchOn<E>`, `Common`,
  `Sizing`, `TextRole`, `Axis`, `FilePickerMode`, `Key` (Part 2, `morph/ui/view.hpp`); `ui::Mounted`
  (`morph/ui/mount.hpp`); `ui::testing::RecordingBackend` (`find`, `all`, `prop`, `dump`, `click`, `drag`, `pick`;
  a stack is `Column`/`Row`, a `Dialog`'s content exists only while it is open); `forms::formView`,
  `FormViewOptions{.submitLabel}` (Part 5); `kanban::testing::fill` (Task K3).
- Produces: `kanban::client::Route{SignIn, Projects, Board}`, `AppOptions{attachments, board}`,
  `AppController(examples::Wiring, AppOptions)` with `route()`, `session()`, `projects()`, `board()`, `rules()`, `detail()`,
  `openProject(ProjectId, Role)`, `backToProjects()`; views `signInView(SessionController&)`,
  `projectsView(AppController&)`, `boardView(AppController&)`, `rulesDialog(RulesController&)`,
  `taskDetailDialog(TaskDetailController&)`, `rootView(AppController&)` — all `-> ui::Node`.

- [ ] **Step 1: Write the failing tests**

Create `examples/kanban/tests/client/test_app_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include "app_controller.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"

using kanban::client::AppController;
using kanban::client::Route;
using kanban::testing::IssuerScope;
using kanban::testing::LocalClient;

TEST_CASE("kanban::client::AppController routes sign-in, projects and board from state", "[kanban][client]") {
    morph::ladder::testkit::DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    AppController app{client.wiring(), {}};
    CHECK(app.route() == Route::SignIn);

    kanban::testing::signIn(client, app.session(), "alice");
    CHECK(app.route() == Route::Projects);
    kanban::testing::submitForm(client, app.projects().createProjectForm(), R"({"name":"Sprint Board"})");
    REQUIRE(client.settle([&app] { return app.projects().projects().size() == 1; }));
    auto const project = app.projects().projects().front();

    app.openProject(kanban::ProjectId{project.id}, project.role);
    CHECK(app.route() == Route::Board);
    REQUIRE(client.settle([&app] { return app.board().board().has_value(); }));
    app.rules().show();

    app.backToProjects();
    CHECK(app.route() == Route::Projects);
    CHECK_FALSE(app.rules().shown());
    REQUIRE(client.settle([&app] { return !app.board().board().has_value(); }));
}
```

Create `examples/kanban/tests/client/test_kanban_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// One RecordingBackend test per screen: the screen's tree carries what it must show, and its controls drive the
// controller. Form fields are filled through the controller's form session (the forms engine renders and tests
// its own fields); the screen's own Submit buttons are clicked here, once the flush that enables them has run.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <utility>

#include "app_controller.hpp"
#include "board_support.hpp"
#include "fake_transfer.hpp"
#include "kanban_client_support.hpp"
#include "testkit/db_fixture.hpp"
#include "views/board_view.hpp"
#include "views/projects_view.hpp"
#include "views/root_view.hpp"
#include "views/sign_in_view.hpp"

namespace {

using kanban::client::AppController;
using kanban::client::AppOptions;
using kanban::testing::IssuerScope;
using kanban::testing::LocalClient;
using morph::ladder::testkit::DbFixture;
using morph::ui::testing::RecordingBackend;

[[nodiscard]] bool shows(RecordingBackend const& backend, std::string const& text) {
    return backend.dump().find(text) != std::string::npos;
}

[[nodiscard]] int button(RecordingBackend const& backend, std::string const& label) {
    auto const found = backend.find("Button", "label", label);
    REQUIRE(found.has_value());
    return *found;
}

// Clicks the button labelled @p label once it is enabled. A form's Submit button follows `ready()` one flush after
// the fields change, and the backend ignores a click on a disabled widget.
void clickWhenEnabled(LocalClient& client, RecordingBackend& backend, std::string const& label) {
    int const target = button(backend, label);
    REQUIRE(client.settle([&backend, target] { return backend.prop(target, "enabled") != "false"; }));
    backend.click(target);
}

// A signed-in app with one project, opened on its board and seeded through the board's forms.
struct BoardScreen {
    explicit BoardScreen(LocalClient& client, AppOptions options = {}) : app{client.wiring(), std::move(options)} {
        kanban::testing::signIn(client, app.session(), "alice");
        auto const project = kanban::testing::seedProject(client, "Sprint Board");
        seeded = kanban::testing::seedBoard(client, app.board(), project);
        app.openProject(project, kanban::Role::Manager);
    }

    AppController app;
    kanban::testing::SeededBoard seeded;
};

}  // namespace

TEST_CASE("kanban view: the sign-in screen's Sign in button submits the Login form", "[kanban][view]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    AppController app{client.wiring(), {}};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, kanban::client::signInView(app.session())};
    REQUIRE(client.settle([&] { return backend.find("Button", "label", "Sign in").has_value(); }));
    CHECK(shows(backend, "Dev-mode login"));

    kanban::testing::fill(app.session().loginForm(), R"({"username":"alice"})");
    clickWhenEnabled(client, backend, "Sign in");

    REQUIRE(client.settle([&app] { return app.session().signedIn(); }));
}

TEST_CASE("kanban view: the root screen follows the route", "[kanban][view]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    AppController app{client.wiring(), {}};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, kanban::client::rootView(app)};
    REQUIRE(client.settle([&] { return shows(backend, "not signed in"); }));

    kanban::testing::signIn(client, app.session(), "alice");

    REQUIRE(client.settle([&] { return shows(backend, "Your projects") && shows(backend, "signed in as alice"); }));
}

TEST_CASE("kanban view: the projects screen creates, lists, opens and shows members", "[kanban][view]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    AppController app{client.wiring(), {}};
    kanban::testing::signIn(client, app.session(), "alice");
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, kanban::client::projectsView(app)};
    REQUIRE(client.settle([&] { return backend.find("Button", "label", "Create project").has_value(); }));

    kanban::testing::fill(app.projects().createProjectForm(), R"({"name":"Sprint Board"})");
    clickWhenEnabled(client, backend, "Create project");
    REQUIRE(client.settle([&] { return shows(backend, "Sprint Board  ·  Manager"); }));

    backend.click(button(backend, "Members"));
    REQUIRE(client.settle([&] { return shows(backend, "Members of Sprint Board") && shows(backend, "alice"); }));
    CHECK(backend.find("Button", "label", "Add member").has_value());
    // alice's own row: the per-row role picker shows her role (a string key, so quoted).
    CHECK(backend.find("Select", "selected", R"("Manager")").has_value());

    backend.click(button(backend, "Open"));
    CHECK(app.route() == kanban::client::Route::Board);
}

TEST_CASE("kanban view: the board is a grid of lanes by columns whose cards move by drag-and-drop", "[kanban][view]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    BoardScreen open{client};
    auto& board = open.app.board();
    board.openNewTask(kanban::ColumnId{open.seeded.done}, kanban::SwimlaneId{open.seeded.lane});
    kanban::testing::submitForm(client, board.createTaskForm(), R"({"title":"Ship it"})");
    REQUIRE(client.settle([&] { return board.cardsIn(open.seeded.done, open.seeded.lane).size() == 1; }));
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, kanban::client::boardView(open.app)};
    REQUIRE(client.settle([&] { return backend.find("Button", "label", "Fix bug").has_value(); }));

    // The board's grid: two columns plus the lane-header column. The column and swimlane forms lay their fields out
    // in grids of their own, which have fewer columns.
    CHECK(backend.find("Grid", "columns", "3").has_value());
    CHECK_FALSE(backend.all("Scroll").empty());
    CHECK(shows(backend, "To Do  (1)"));
    CHECK(shows(backend, "Done  (1)"));
    CHECK(shows(backend, "role: Manager"));

    CHECK(backend.drag(button(backend, "Fix bug"), button(backend, "Ship it")));

    REQUIRE(client.settle([&] { return board.cardsIn(open.seeded.done, open.seeded.lane).size() == 2; }));
    CHECK(board.cardsIn(open.seeded.done, open.seeded.lane).front().label == "Fix bug");
    REQUIRE(client.settle([&] { return shows(backend, "To Do  (0)") && shows(backend, "Done  (2)"); }));
}

TEST_CASE("kanban view: a drop carrying a non-task key is refused and moves nothing", "[kanban][view]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    BoardScreen open{client};
    auto& board = open.app.board();
    RecordingBackend backend;
    namespace ui = morph::ui;
    morph::ui::Mounted const mounted{
        client.runtime, backend,
        ui::column({.children = {kanban::client::boardView(open.app),
                                 ui::button({.label = "foreign",
                                             .common = {.dragKey = std::optional<ui::Key>{ui::Key{std::string{"7"}}}}}),
                                 ui::button({.label = "zero",
                                             .common = {.dragKey = std::optional<ui::Key>{ui::Key{std::int64_t{0}}}}})}})};
    REQUIRE(client.settle([&] { return backend.find("Button", "label", "Fix bug").has_value(); }));

    CHECK_FALSE(backend.drag(button(backend, "foreign"), button(backend, "Fix bug")));
    CHECK_FALSE(backend.drag(button(backend, "zero"), button(backend, "Fix bug")));

    CHECK(board.lastMoveOpId().empty());
    CHECK_FALSE(board.movesPending());
}

TEST_CASE("kanban view: the rules dialog lists, adds and removes rules", "[kanban][view]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    BoardScreen open{client};
    auto& rules = open.app.rules();
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, kanban::client::rulesDialog(rules)};

    rules.show();
    REQUIRE(client.settle([&] { return backend.find("Button", "label", "Add rule").has_value(); }));
    CHECK(shows(backend, "Automation rules"));
    kanban::testing::fill(rules.createRuleForm(), R"({"triggerColumnId":)" + std::to_string(open.seeded.done) +
                                                       R"(,"mutationType":"AddTag","mutationValue":"shipped"})");
    clickWhenEnabled(client, backend, "Add rule");
    REQUIRE(client.settle([&] { return shows(backend, R"(when moved to "Done": AddTag "shipped")"); }));

    backend.click(button(backend, "Remove"));
    REQUIRE(client.settle([&rules] { return rules.rules().empty(); }));
    backend.click(button(backend, "Close"));
    CHECK_FALSE(rules.shown());
}

TEST_CASE("kanban view: the task dialog adds a comment and hands a picked file to the transfer", "[kanban][view]") {
    DbFixture const fixture;
    IssuerScope const issuer;
    LocalClient client;
    auto const transfer = std::make_shared<kanban::testing::FakeTransfer>(client.owner);
    BoardScreen open{client, AppOptions{.attachments = transfer, .board = {}}};
    auto& detail = open.app.detail();
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, kanban::client::taskDetailDialog(detail)};

    detail.openTask(kanban::TaskId{open.seeded.task});
    REQUIRE(client.settle([&] { return backend.find("Button", "label", "Add comment").has_value(); }));
    CHECK(shows(backend, "Fix bug"));
    kanban::testing::fill(detail.addCommentForm(), R"({"body":"looks good"})");
    clickWhenEnabled(client, backend, "Add comment");
    // A Text, not the form's TextInput, which held the typed body before the submission.
    REQUIRE(client.settle([&] { return backend.find("Text", "text", "looks good").has_value(); }));

    auto const pickers = backend.all("FilePicker");
    REQUIRE(pickers.size() == 1);
    backend.pick(pickers.front(), "/tmp/report.pdf");
    REQUIRE(transfer->uploads.size() == 1);
    CHECK(transfer->uploads.front().localPath == std::filesystem::path{"/tmp/report.pdf"});
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_tests`
Expected: compile error, `'app_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/app_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/reactive/signal.hpp>

#include "app/wiring.hpp"
#include "attachments/attachment_transfer.hpp"
#include "controllers/board_controller.hpp"
#include "controllers/projects_controller.hpp"
#include "controllers/rules_controller.hpp"
#include "controllers/session_controller.hpp"
#include "controllers/task_detail_controller.hpp"

namespace kanban::client {

/// @brief The screen the app shows.
enum class Route : std::uint8_t { SignIn, Projects, Board };

/// @brief What the app is configured with besides its wiring.
struct AppOptions {
    /// @brief The attachment side channel's client; null disables attachments.
    std::shared_ptr<IAttachmentTransfer> attachments;
    /// @brief The board's offline queue and poll period.
    BoardOptions board;
};

/// @brief The kanban client: its five controllers, and which screen they add up to.
class AppController {
public:
    /// @param wiring The runtime, scheduler, bridge and owner every controller uses.
    /// @param options Attachments and board options.
    AppController(morph::examples::Wiring wiring, AppOptions options);

    /// @brief The screen to show. Tracked: signed out is sign-in, an open board is the board, else projects.
    /// @return The route.
    [[nodiscard]] Route route() const { return _route.get(); }
    /// @brief The sign-in controller. @return It.
    [[nodiscard]] SessionController& session() noexcept { return _session; }
    /// @brief The project list controller. @return It.
    [[nodiscard]] ProjectsController& projects() noexcept { return _projects; }
    /// @brief The board controller. @return It.
    [[nodiscard]] BoardController& board() noexcept { return _board; }
    /// @brief The rules dialog controller. @return It.
    [[nodiscard]] RulesController& rules() noexcept { return _rules; }
    /// @brief The task dialog controller. @return It.
    [[nodiscard]] TaskDetailController& detail() noexcept { return _detail; }

    /// @brief Opens a project's board.
    /// @param project The project.
    /// @param myRole The caller's role there.
    void openProject(ProjectId project, Role myRole);
    /// @brief Leaves the board for the project list, closing its dialogs.
    void backToProjects();

private:
    SessionController _session;
    ProjectsController _projects;
    BoardController _board;
    RulesController _rules;
    TaskDetailController _detail;
    morph::reactive::Computed<Route> _route;
};

}  // namespace kanban::client
```

Create `examples/kanban/app/app_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "app_controller.hpp"

#include <utility>

namespace kanban::client {

AppController::AppController(morph::examples::Wiring wiring, AppOptions options)
    : _session{wiring},
      _projects{wiring, _session},
      _board{wiring, std::move(options.board)},
      _rules{wiring, _board},
      _detail{wiring, _board, std::move(options.attachments)},
      _route{wiring.runtime, [this] {
                 if (!_session.signedIn()) {
                     return Route::SignIn;
                 }
                 return _board.isOpen() ? Route::Board : Route::Projects;
             }} {}

void AppController::openProject(ProjectId project, Role myRole) {
    _projects.hideMembers();
    _board.open(project, myRole);
}

void AppController::backToProjects() {
    _rules.hide();
    _detail.close();
    _board.close();
    _projects.refresh();
}

}  // namespace kanban::client
```

Create `examples/kanban/app/views/sign_in_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/session_controller.hpp"

namespace kanban::client {

/// @brief The sign-in screen: the schema-driven `Login` form.
/// @param session Its controller; it outlives the mounted view.
/// @return The screen.
[[nodiscard]] morph::ui::Node signInView(SessionController& session);

}  // namespace kanban::client
```

Create `examples/kanban/app/views/sign_in_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/sign_in_view.hpp"

#include <morph/forms/engine/form_view.hpp>

namespace kanban::client {

morph::ui::Node signInView(SessionController& session) {
    namespace ui = morph::ui;
    return ui::column({
        .children =
            {
                ui::text({.text = "Sign in", .role = ui::TextRole::Heading}),
                ui::text({.text = "Dev-mode login: a username, no password. The token the server mints for it is "
                                  "real, server-signed and checked on every later action.",
                          .role = ui::TextRole::Muted}),
                morph::forms::formView(session.loginForm(), {.submitLabel = "Sign in"}),
            },
        .gap = 1,
    });
}

}  // namespace kanban::client
```

Create `examples/kanban/app/views/projects_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "app_controller.hpp"

namespace kanban::client {

/// @brief The project list, the create-project form, and the members panel with its role pickers.
/// @param app The app; it outlives the mounted view.
/// @return The screen.
[[nodiscard]] morph::ui::Node projectsView(AppController& app);

}  // namespace kanban::client
```

Create `examples/kanban/app/views/projects_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/projects_view.hpp"

#include <morph/forms/engine/form_view.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "support/board_format.hpp"

namespace kanban::client {

namespace {

namespace ui = morph::ui;
using morph::reactive::Signal;

ui::Node projectRowView(AppController& app, Signal<ProjectRow> const& row) {
    return ui::row({
        .children =
            {
                ui::text({.text = [&row] { return row.get().label; },
                          .common = {.layout = {.width = ui::Sizing::stretch()}}}),
                ui::button({.label = "Open",
                            .onClick =
                                [&app, &row] {
                                    ProjectRow const project = row.peek();
                                    app.openProject(ProjectId{project.id}, project.role);
                                }}),
                ui::button({.label = "Members",
                            .onClick =
                                [&app, &row] {
                                    ProjectRow const project = row.peek();
                                    app.projects().showMembers(ProjectId{project.id}, project.name);
                                }}),
            },
        .gap = 1,
    });
}

ui::Node memberRowView(ProjectsController& projects, Signal<MemberRow> const& member) {
    std::vector<ui::SelectOption> options;
    for (std::string const& name : ProjectsController::roleNames()) {
        options.push_back(ui::SelectOption{.key = ui::Key{name}, .label = name});
    }
    return ui::row({
        .children =
            {
                ui::text({.text = [&member] { return member.get().principal; },
                          .common = {.layout = {.width = ui::Sizing::stretch()}}}),
                ui::select({.options = std::move(options),
                            .selected = [&member]() -> std::optional<ui::Key> {
                                return ui::Key{roleLabel(member.get().role)};
                            },
                            .onSelect =
                                [&projects, &member](ui::Key const& key) {
                                    if (auto const* const role = std::get_if<std::string>(&key); role != nullptr) {
                                        projects.setRole(member.peek().principal, roleFromString(*role));
                                    }
                                }}),
                ui::button({.label = "Remove",
                            .onClick = [&projects, &member] { projects.removeMember(member.peek().principal); }}),
            },
        .gap = 1,
    });
}

ui::Node membersPanel(ProjectsController& projects) {
    return ui::panel({
        .title = [&projects] { return projects.membersTitle(); },
        .padding = 1,
        .child = ui::column({
            .children =
                {
                    ui::forEach<MemberRow>(
                        [&projects] { return projects.members(); },
                        [](MemberRow const& member) { return ui::Key{member.principal}; },
                        [&projects](Signal<MemberRow> const& member) { return memberRowView(projects, member); }),
                    morph::forms::formView(projects.addMemberForm(), {.submitLabel = "Add member"}),
                    ui::button({.label = "Close", .onClick = [&projects] { projects.hideMembers(); }}),
                },
            .gap = 1,
        }),
        .common = {.visible = [&projects] { return projects.membersShown(); },
                   .layout = {.width = ui::Sizing::stretch()}},
    });
}

}  // namespace

ui::Node projectsView(AppController& app) {
    ProjectsController& projects = app.projects();
    return ui::column({
        .children =
            {
                ui::text({.text = [&projects] { return projects.errorText(); }, .role = ui::TextRole::Error}),
                ui::row({
                    .children =
                        {
                            ui::column({
                                .children =
                                    {
                                        ui::text({.text = "Your projects", .role = ui::TextRole::Heading}),
                                        morph::forms::formView(projects.createProjectForm(),
                                                               {.submitLabel = "Create project"}),
                                        ui::forEach<ProjectRow>(
                                            [&projects] { return projects.projects(); },
                                            [](ProjectRow const& project) { return ui::Key{project.id}; },
                                            [&app](Signal<ProjectRow> const& row) { return projectRowView(app, row); }),
                                    },
                                .gap = 1,
                                .common = {.layout = {.width = ui::Sizing::stretch()}},
                            }),
                            membersPanel(projects),
                        },
                    .gap = 2,
                }),
            },
        .gap = 1,
    });
}

}  // namespace kanban::client
```

Create `examples/kanban/app/views/board_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "app_controller.hpp"

namespace kanban::client {

/// @brief The board screen: header, sync banners, the column and swimlane forms, the grid of lanes by columns with
///        draggable cards, the activity panel, and the new-task, rules and task dialogs.
/// @param app The app; it outlives the mounted view.
/// @return The screen.
[[nodiscard]] morph::ui::Node boardView(AppController& app);

/// @brief The rules dialog.
/// @param rules Its controller; it outlives the mounted view.
/// @return The dialog.
[[nodiscard]] morph::ui::Node rulesDialog(RulesController& rules);

/// @brief The task dialog: comments, the add-comment form, attachments with download and upload pickers.
/// @param detail Its controller; it outlives the mounted view.
/// @return The dialog.
[[nodiscard]] morph::ui::Node taskDetailDialog(TaskDetailController& detail);

}  // namespace kanban::client
```

Create `examples/kanban/app/views/board_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/board_view.hpp"

#include <cstdint>
#include <morph/forms/engine/form_view.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kanban::client {

namespace {

namespace ui = morph::ui;
using morph::reactive::Signal;

[[nodiscard]] bool acceptsTask(ui::Key const& key) { return BoardController::taskOf(key).has_value(); }

ui::Node cardView(AppController& app, Signal<CardRow> const& card) {
    BoardController& board = app.board();
    TaskDetailController& detail = app.detail();
    return ui::button({
        .label = [&card] { return card.get().label; },
        .onClick = [&detail, &card] { detail.openTask(TaskId{card.peek().id}); },
        .common = {.dragKey = [&card]() -> std::optional<ui::Key> { return ui::Key{card.get().id}; },
                   .accepts = acceptsTask,
                   .onDrop =
                       [&board, &card](ui::Key const& key) {
                           if (std::optional<TaskId> const task = BoardController::taskOf(key)) {
                               CardRow const target = card.peek();
                               board.moveTask(*task, ColumnId{target.columnId}, SwimlaneId{target.laneId},
                                              target.position);
                           }
                       }},
    });
}

ui::Node cellView(AppController& app, ColumnHead const& column, LaneHead const& lane) {
    BoardController& board = app.board();
    std::int64_t const columnId = column.id;
    std::optional<std::int64_t> const laneId = lane.id;
    return ui::column({
        .children =
            {
                ui::button({.label = "+ task",
                            .onClick =
                                [&board, columnId, laneId] {
                                    if (laneId) {
                                        board.openNewTask(ColumnId{columnId}, SwimlaneId{*laneId});
                                    }
                                },
                            .common = {.enabled = lane.canAddTask}}),
                ui::forEach<CardRow>([&board, columnId, laneId] { return board.cardsIn(columnId, laneId); },
                                     [](CardRow const& card) { return ui::Key{card.id}; },
                                     [&app](Signal<CardRow> const& card) { return cardView(app, card); }),
            },
        .gap = 0,
        .common = {.accepts = acceptsTask,
                   .onDrop =
                       [&board, columnId, laneId](ui::Key const& key) {
                           if (std::optional<TaskId> const task = BoardController::taskOf(key)) {
                               board.dropAtEnd(*task, ColumnId{columnId}, laneId);
                           }
                       }},
    });
}

ui::Node gridView(AppController& app, BoardShape const& shape) {
    BoardController& board = app.board();
    std::vector<ui::GridCell> cells;
    cells.push_back(ui::GridCell{.node = ui::spacer()});
    for (ColumnHead const& column : shape.columns) {
        std::int64_t const columnId = column.id;
        cells.push_back(ui::GridCell{.node = ui::text({.text = [&board, columnId] { return board.headerOf(columnId); },
                                                       .role = ui::TextRole::Heading})});
    }
    for (LaneHead const& lane : shape.lanes) {
        cells.push_back(ui::GridCell{.node = ui::text({.text = lane.label, .role = ui::TextRole::Heading})});
        for (ColumnHead const& column : shape.columns) {
            cells.push_back(ui::GridCell{.node = cellView(app, column, lane)});
        }
    }
    return ui::grid({.columns = static_cast<int>(shape.columns.size()) + 1, .cells = std::move(cells), .gap = 1});
}

ui::Node activityPanel(BoardController& board) {
    return ui::panel({
        .title = "Activity",
        .padding = 1,
        .child = ui::forEach<ActivityRow>(
            [&board] { return board.activity(); }, [](ActivityRow const& row) { return ui::Key{row.index}; },
            [](Signal<ActivityRow> const& row) {
                return ui::column({.children = {ui::text({.text = [&row] { return row.get().summary; }}),
                                                ui::text({.text = [&row] { return row.get().meta; },
                                                          .role = ui::TextRole::Muted})}});
            }),
        .common = {.layout = {.width = ui::Sizing::fixed(32)}},
    });
}

ui::Node newTaskDialog(BoardController& board) {
    return ui::dialog({.open = [&board] { return board.newTaskOpen(); },
                       .title = [&board] { return board.newTaskTitle(); },
                       .child = morph::forms::formView(board.createTaskForm(), {.submitLabel = "Create task"}),
                       .onDismiss = [&board] { board.closeNewTask(); }});
}

}  // namespace

ui::Node boardView(AppController& app) {
    BoardController& board = app.board();
    return ui::column({
        .children =
            {
                ui::row({.children = {ui::button({.label = "< Back", .onClick = [&app] { app.backToProjects(); }}),
                                      ui::text({.text = [&board] { return board.title(); },
                                                .role = ui::TextRole::Heading}),
                                      ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                      ui::button({.label = "Rules", .onClick = [&app] { app.rules().show(); }}),
                                      ui::text({.text = [&board] { return board.roleText(); },
                                                .role = ui::TextRole::Muted})},
                         .gap = 1}),
                ui::text({.text = [&board] { return board.deadLetterText(); }, .role = ui::TextRole::Error}),
                ui::text({.text = [&board] { return board.syncText(); }, .role = ui::TextRole::Muted}),
                ui::row({.children = {ui::panel({.title = "New column",
                                                 .padding = 1,
                                                 .child = morph::forms::formView(board.createColumnForm(),
                                                                                 {.submitLabel = "Add column"})}),
                                      ui::panel({.title = "New swimlane",
                                                 .padding = 1,
                                                 .child = morph::forms::formView(board.createSwimlaneForm(),
                                                                                 {.submitLabel = "Add swimlane"})})},
                         .gap = 2}),
                ui::text({.text = [&board] { return board.errorText(); }, .role = ui::TextRole::Error}),
                ui::row({.children = {ui::forEach<BoardShape>(
                                          [&board] { return board.shapes(); },
                                          [](BoardShape const& shape) { return ui::Key{shape.key}; },
                                          [&app](Signal<BoardShape> const& shape) {
                                              return ui::scroll({.child = gridView(app, shape.peek()),
                                                                 .axis = ui::Axis::Horizontal});
                                          }),
                                      activityPanel(board)},
                         .gap = 2}),
                newTaskDialog(board),
                rulesDialog(app.rules()),
                taskDetailDialog(app.detail()),
            },
        .gap = 1,
    });
}

ui::Node rulesDialog(RulesController& rules) {
    return ui::dialog({
        .open = [&rules] { return rules.shown(); },
        .title = "Automation rules",
        .child = ui::column({
            .children =
                {
                    ui::forEach<RuleRow>(
                        [&rules] { return rules.rules(); }, [](RuleRow const& rule) { return ui::Key{rule.id}; },
                        [&rules](Signal<RuleRow> const& rule) {
                            return ui::row({.children = {ui::text({.text = [&rule] { return rule.get().text; },
                                                                   .common = {.layout = {.width =
                                                                                             ui::Sizing::stretch()}}}),
                                                         ui::button({.label = "Remove", .onClick = [&rules, &rule] {
                                                                         rules.remove(RuleId{rule.peek().id});
                                                                     }})},
                                            .gap = 1});
                        }),
                    morph::forms::formView(rules.createRuleForm(), {.submitLabel = "Add rule"}),
                    ui::text({.text = [&rules] { return rules.errorText(); }, .role = ui::TextRole::Error}),
                    ui::button({.label = "Close", .onClick = [&rules] { rules.hide(); }}),
                },
            .gap = 1,
        }),
        .onDismiss = [&rules] { rules.hide(); },
    });
}

ui::Node taskDetailDialog(TaskDetailController& detail) {
    bool const transfers = detail.attachmentsAvailable();
    return ui::dialog({
        .open = [&detail] { return detail.isOpen(); },
        .title = [&detail] { return detail.title(); },
        .child = ui::column({
            .children =
                {
                    ui::text({.text = "Comments", .role = ui::TextRole::Heading}),
                    ui::forEach<CommentRow>(
                        [&detail] { return detail.comments(); }, [](CommentRow const& row) { return ui::Key{row.index}; },
                        [](Signal<CommentRow> const& comment) {
                            return ui::column(
                                {.children = {ui::text({.text = [&comment] { return comment.get().principal; },
                                                        .role = ui::TextRole::Muted}),
                                              ui::text({.text = [&comment] { return comment.get().body; }})}});
                        }),
                    morph::forms::formView(detail.addCommentForm(), {.submitLabel = "Add comment"}),
                    ui::text({.text = "Attachments", .role = ui::TextRole::Heading}),
                    ui::forEach<AttachmentRow>(
                        [&detail] { return detail.attachments(); },
                        [](AttachmentRow const& row) { return ui::Key{row.id}; },
                        [&detail, transfers](Signal<AttachmentRow> const& attachment) {
                            return ui::row(
                                {.children = {ui::text({.text = [&attachment] { return attachment.get().line; },
                                                        .common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                              ui::filePicker({.path = std::string{},
                                                              .mode = ui::FilePickerMode::Save,
                                                              .onPicked =
                                                                  [&detail, &attachment](std::string const& path) {
                                                                      detail.download(attachment.peek().id, path);
                                                                  },
                                                              .common = {.enabled = transfers}})},
                                 .gap = 1});
                        }),
                    ui::filePicker({.path = std::string{},
                                    .mode = ui::FilePickerMode::Open,
                                    .onPicked = [&detail](std::string const& path) { detail.upload(path); },
                                    .common = {.enabled = transfers}}),
                    ui::text({.text = [&detail] { return detail.statusText(); }, .role = ui::TextRole::Muted}),
                    ui::button({.label = "Close", .onClick = [&detail] { detail.close(); }}),
                },
            .gap = 1,
        }),
        .onDismiss = [&detail] { detail.close(); },
    });
}

}  // namespace kanban::client
```

Create `examples/kanban/app/views/root_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "app_controller.hpp"

namespace kanban::client {

/// @brief The whole app: the header line and the screen the route names.
/// @param app The app; it outlives the mounted view.
/// @return The root node.
[[nodiscard]] morph::ui::Node rootView(AppController& app);

}  // namespace kanban::client
```

Create `examples/kanban/app/views/root_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/root_view.hpp"

#include "views/board_view.hpp"
#include "views/projects_view.hpp"
#include "views/sign_in_view.hpp"

namespace kanban::client {

morph::ui::Node rootView(AppController& app) {
    namespace ui = morph::ui;
    SessionController& session = app.session();
    return ui::column({
        .children =
            {
                ui::row({.children = {ui::text({.text = "kanban", .role = ui::TextRole::Heading}),
                                      ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                      ui::text({.text = [&session] { return session.headerText(); },
                                                .role = ui::TextRole::Muted})},
                         .gap = 1}),
                ui::switchOn<Route>([&app] { return app.route(); },
                                    {{Route::SignIn, signInView(session)},
                                     {Route::Projects, projectsView(app)},
                                     {Route::Board, boardView(app)}}),
            },
        .gap = 1,
    });
}

}  // namespace kanban::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][client],[kanban][view]"
git grep -n -E '#include <Q|morph/qt|morph/tui|morph/qt_quick' -- examples/kanban/app; test $? -eq 1 && echo "app is toolkit-free"
git grep -n 'morph/ui/' -- examples/kanban/app/controllers; test $? -eq 1 && echo "controllers are UI-free"
```

Expected: PASS; both checks print their line.

Mutation checks, each restored afterwards:
- Make `acceptsTask` return `true`. Expected FAIL in "a drop carrying a non-task key is refused and moves nothing"
  (`drag` reports the drop accepted).
- In `cardView`'s `onDrop`, use `target.position + 1`. Expected FAIL in "the board is a grid of lanes by columns
  whose cards move by drag-and-drop" (Fix bug lands after Ship it).

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/app_controller.hpp examples/kanban/app/app_controller.cpp examples/kanban/app/views \
        examples/kanban/tests/client/test_app_controller.cpp examples/kanban/tests/client/test_kanban_views.cpp
git commit -m "wip(kanban): AppController and the views: a grid board with drag-and-drop on both frontends

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K13: The application, the composition root, the Qt attachment transfer, and the frontend smoke tests

`kanban::client::makeApplication(ctx, env)` connects (Part 6's `connect`: local or remote, on the frontend's
executor), installs the local-mode token issuer for as long as a local app lives, and builds the `AppController`.
Kanban's two knobs come from the environment, so `ui/main.cpp` is exactly spec 4 §3's composition root:

- **The attachment transfer.** Without `--server` there is none (a local app has no side channel). With it, the side
  channel is `KANBAN_ATTACHMENT_SERVER` when set, else `attachmentServerFor(*env.server)` (the server's host, port
  8769); on the `"qt"` frontend the transfer is Qt's network stack when the build has it, otherwise the POSIX HTTP
  client.
- **The offline queue.** Only against a server (a local app has no network to lose), and only in a build with
  `MORPH_BUILD_OFFLINE_SQLITE`: its file is `KANBAN_OFFLINE_QUEUE_DB`, as the Qt client read it, else
  `kanban-offline-queue.db`.

Both variables are read through `ui::processEnvironment()`. The Qt transfer stays out of `app/`'s headers: its own
header is Qt-free (a factory returning `IAttachmentTransfer`), and `qt/qt_attachment_transfer.cpp` is compiled into
`ladder_kanban_app` only when Qt Quick is built, with `KANBAN_HAS_QT_TRANSFER` defined to `0` or `1` so the
application knows whether it may call it — the same shape as Part 6's `MORPH_EXAMPLES_TRANSPORT_QT`.

**Files:**
- Create: `examples/kanban/app/kanban_application.hpp`, `examples/kanban/app/kanban_application.cpp`
- Create: `examples/kanban/ui/main.cpp`
- Create: `examples/kanban/qt/qt_attachment_transfer.hpp`, `examples/kanban/qt/qt_attachment_transfer.cpp`
- Modify: `examples/kanban/CMakeLists.txt` — append the Qt-transfer block below
- Test: `examples/kanban/tests/smoke/test_kanban_frontends.cpp`

**Interfaces:**
- Consumes: `morph::examples::AppEnvironment`, `connect`, `LocalSetup`, `Connection::bridge()`/`callbacks()`,
  `Wiring` (Part 6); `ui::AppContext`, `ui::Application`, `ui::selectFrontend`, `ui::FrontendOption`,
  `ui::processEnvironment()` (Part 2); `morph::tui::frontendOption()` (Part 3, `<morph/tui/frontend.hpp>`);
  `morph::qt_quick::frontendOption(int&, char**)` (Part 4, `<morph/qt_quick/frontend.hpp>`);
  `morph::examples::testing::runFrontendSmoke`, `SmokeFrontend` (Part 6); `AppController`, `AppOptions`,
  `rootView`, `HttpAttachmentTransfer`, `attachmentServerFor`, `OfflineConfig`, `BoardOptions` (Tasks K5–K12);
  `kanban::db::setup`, `kanban::auth::setTokenIssuer`.
- Produces: `kanban::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&) ->
  std::unique_ptr<ui::Application>`; `kanban::qt::makeQtAttachmentTransfer(std::string const& baseUrl,
  exec::IExecutor& callbacks) -> std::shared_ptr<client::IAttachmentTransfer>`; the `kanban` binary.

- [ ] **Step 1: Write the failing test**

Create `examples/kanban/tests/smoke/test_kanban_frontends.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The application mounts and quits on every frontend this build has (spec 4 §7 rule 5). This file is
// ladder_kanban_smoke_tests, a binary of its own: its main owns no Qt application object.

#if MORPH_EXAMPLE_HAS_TUI || MORPH_EXAMPLE_HAS_QT_QUICK

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"
#include "kanban_application.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/frontend_smoke.hpp"

namespace {

using morph::examples::testing::SmokeFrontend;

// The test binary's own database, so the app's local setup re-applies the schema the fixture already has.
[[nodiscard]] morph::examples::AppEnvironment smokeEnvironment() {
    morph::examples::AppEnvironment env;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read on the test thread before the app starts any thread.
    env.db = morph::ladder::testkit::DbFixture::computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"));
    env.user = "alice";
    return env;
}

void smoke(SmokeFrontend frontend) {
    morph::ladder::testkit::DbFixture const fixture;
    auto const env = smokeEnvironment();
    morph::examples::testing::runFrontendSmoke(
        [&env](morph::ui::AppContext& ctx) { return kanban::client::makeApplication(ctx, env); }, frontend);
}

}  // namespace

#if MORPH_EXAMPLE_HAS_TUI
TEST_CASE("kanban mounts and quits on the TUI", "[kanban][smoke]") { smoke(SmokeFrontend::Tui); }
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
TEST_CASE("kanban mounts and quits on Qt Quick", "[kanban][smoke]") { smoke(SmokeFrontend::QtQuick); }
#endif

#endif
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_kanban_smoke_tests` (`morph_add_rung` builds
`tests/smoke/*.cpp` into that binary, Part 6).
Expected: compile error, `'kanban_application.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/kanban/app/kanban_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"

namespace kanban::client {

/// @brief Builds the kanban application on a frontend's context: connects (local or `--server`), in local mode
///        installs the token issuer sign-in mints from, and against a server adds the attachment side channel
///        (`KANBAN_ATTACHMENT_SERVER`, else the server's host on port 8769) and, in a build with
///        `MORPH_BUILD_OFFLINE_SQLITE`, the offline move queue (`KANBAN_OFFLINE_QUEUE_DB`, else
///        `kanban-offline-queue.db`).
/// @param ctx The frontend's context. Borrowed: it outlives the application.
/// @param env `--server`, `--db`, `--user`.
/// @return The application.
/// @throws morph::examples::TransportError when `--server` is given and this frontend has no transport for it.
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env);

}  // namespace kanban::client
```

Create `examples/kanban/app/kanban_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "kanban_application.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "app/transport.hpp"
#include "app/wiring.hpp"
#include "app_controller.hpp"
#include "attachments/attachment_transfer.hpp"
#include "controllers/offline_config.hpp"
#include "http/http_attachment_transfer.hpp"
#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/db/database.hpp"
#include "views/root_view.hpp"

#if KANBAN_HAS_QT_TRANSFER
#include "qt_attachment_transfer.hpp"
#endif

namespace kanban::client {

namespace {

constexpr std::string_view kDefaultDatabase = "DRIVER=SQLite3;Database=kanban.db;Timeout=5000";
constexpr std::string_view kDefaultOfflineQueue = "kanban-offline-queue.db";
// A local app is its own server: it signs and checks its own tokens, so a fixed development secret serves.
constexpr std::string_view kLocalSecret = "local-mode-development-secret";
constexpr std::size_t kLocalWorkers = 4;

// Installs the issuer `Login` mints tokens from while a local app lives, and removes it after.
class LocalIssuer {
public:
    explicit LocalIssuer(bool install) : _installed{install} {
        if (_installed) {
            auth::setTokenIssuer(
                std::make_shared<morph::session::TokenIssuer>(std::string{kLocalSecret}, morph::session::hmacSha256));
        }
    }
    ~LocalIssuer() {
        if (_installed) {
            auth::setTokenIssuer(nullptr);
        }
    }
    LocalIssuer(LocalIssuer const&) = delete;
    LocalIssuer& operator=(LocalIssuer const&) = delete;
    LocalIssuer(LocalIssuer&&) = delete;
    LocalIssuer& operator=(LocalIssuer&&) = delete;

private:
    bool _installed;
};

[[nodiscard]] morph::examples::AppEnvironment withDatabase(morph::examples::AppEnvironment env) {
    if (env.db.empty()) {
        env.db = std::string{kDefaultDatabase};
    }
    return env;
}

// The side channel's client: none without a server; Qt's network stack on the "qt" frontend when this build has it,
// the POSIX HTTP client otherwise.
[[nodiscard]] std::shared_ptr<IAttachmentTransfer> transferFor(morph::ui::AppContext& ctx,
                                                               morph::examples::AppEnvironment const& env) {
    if (!env.server) {
        return nullptr;
    }
    std::optional<std::string> url = morph::ui::processEnvironment()("KANBAN_ATTACHMENT_SERVER");
    if (!url) {
        url = attachmentServerFor(*env.server);
    }
    if (!url) {
        return nullptr;
    }
#if KANBAN_HAS_QT_TRANSFER
    if (ctx.frontendName() == "qt") {
        return qt::makeQtAttachmentTransfer(*url, ctx.executor());
    }
#endif
    return std::make_shared<HttpAttachmentTransfer>(*url, ctx.executor());
}

// The durable queue for moves made while the server is unreachable: only against a server, and only in a build
// that has the queue.
[[nodiscard]] std::optional<OfflineConfig> offlineQueueFor(morph::examples::AppEnvironment const& env) {
#ifdef MORPH_BUILD_OFFLINE_SQLITE
    if (env.server) {
        std::optional<std::string> const path = morph::ui::processEnvironment()("KANBAN_OFFLINE_QUEUE_DB");
        return OfflineConfig{.queuePath = path.value_or(std::string{kDefaultOfflineQueue})};
    }
#else
    static_cast<void>(env);
#endif
    return std::nullopt;
}

class KanbanApplication final : public morph::ui::Application {
public:
    KanbanApplication(morph::ui::AppContext& ctx, morph::examples::AppEnvironment const& env)
        : _issuer{!env.server.has_value()},
          _connection{morph::examples::connect(
              ctx, withDatabase(env),
              morph::examples::LocalSetup{.setupDatabase = [](std::string const& database) { db::setup(database); },
                                          .workers = kLocalWorkers})},
          _app{morph::examples::Wiring{.runtime = ctx.runtime(),
                                       .scheduler = ctx.scheduler(),
                                       .bridge = _connection->bridge(),
                                       .callbacks = _connection->callbacks()},
               AppOptions{.attachments = transferFor(ctx, env),
                          .board = BoardOptions{.offline = offlineQueueFor(env),
                                                .pollEvery = std::chrono::milliseconds{3000}}}} {}

    [[nodiscard]] morph::ui::Node view() override { return rootView(_app); }

private:
    LocalIssuer _issuer;
    std::unique_ptr<morph::examples::Connection> _connection;
    AppController _app;
};

}  // namespace

std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                        morph::examples::AppEnvironment const& env) {
    return std::make_unique<KanbanApplication>(ctx, env);
}

}  // namespace kanban::client
```

Create `examples/kanban/qt/qt_attachment_transfer.hpp` — Qt-free, so the application can include it:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/executor.hpp>
#include <string>

#include "attachments/attachment_transfer.hpp"

namespace kanban::qt {

/// @brief `IAttachmentTransfer` over `QNetworkAccessManager`, for the Qt Quick frontend: requests run on the Qt
///        event loop that frontend drives, so it needs no thread of its own and works on every Qt platform. Built
///        only with Qt Quick (`KANBAN_HAS_QT_TRANSFER`).
/// @param baseUrl The side channel, e.g. `http://127.0.0.1:8769`.
/// @param callbacks Owns the returned completions: the frontend's executor. Borrowed.
/// @return The transfer; destroying it aborts requests in flight, whose completions never settle.
[[nodiscard]] std::shared_ptr<client::IAttachmentTransfer> makeQtAttachmentTransfer(std::string const& baseUrl,
                                                                                    morph::exec::IExecutor& callbacks);

}  // namespace kanban::qt
```

Create `examples/kanban/qt/qt_attachment_transfer.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "qt_attachment_transfer.hpp"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QUrl>
#include <cstdint>
#include <exception>
#include <morph/core/completion.hpp>
#include <stdexcept>
#include <utility>

namespace kanban::qt {

namespace {

template <typename T>
[[nodiscard]] std::shared_ptr<typename morph::async::Completion<T>::Promise> sharedPromise(
    typename morph::async::Completion<T>::Promise promise) {
    return std::make_shared<typename morph::async::Completion<T>::Promise>(std::move(promise));
}

[[nodiscard]] std::exception_ptr failure(std::string const& message) {
    return std::make_exception_ptr(std::runtime_error{message});
}

class QtAttachmentTransfer final : public client::IAttachmentTransfer {
public:
    QtAttachmentTransfer(std::string const& baseUrl, morph::exec::IExecutor& callbacks)
        : _baseUrl{QString::fromStdString(baseUrl)},
          _callbacks{&callbacks},
          _network{std::make_unique<QNetworkAccessManager>()} {}

    // Qt carries HTTP on every platform it runs on.
    [[nodiscard]] bool available() const noexcept override { return true; }

    [[nodiscard]] morph::async::Completion<client::UploadedBlob> upload(client::UploadRequest request) override {
        auto settleable = morph::async::Completion<client::UploadedBlob>::makeSettleable(_callbacks);
        auto promise = sharedPromise<client::UploadedBlob>(std::move(settleable.second));
        QFile file{QString::fromStdString(request.localPath.string())};
        if (!file.open(QIODevice::ReadOnly)) {
            promise->reject(failure("could not open '" + request.localPath.string() + "' for reading"));
            return std::move(settleable.first);
        }
        QByteArray const bytes = file.readAll();
        std::string const contentType = client::contentTypeFor(request.localPath);
        QNetworkRequest post{QUrl{_baseUrl + QStringLiteral("/attachments")}};
        post.setRawHeader("X-Attachment-Content-Type", QByteArray::fromStdString(contentType));
        post.setRawHeader("Authorization", QByteArray::fromStdString("Bearer " + request.bearerToken));
        QNetworkReply* const reply = _network->post(post, bytes);
        QObject::connect(reply, &QNetworkReply::finished, reply,
                         [reply, promise, filename = request.localPath.filename().string(), contentType,
                          size = static_cast<std::int64_t>(bytes.size())] {
                             reply->deleteLater();
                             if (reply->error() != QNetworkReply::NoError) {
                                 promise->reject(failure("upload failed: " + reply->errorString().toStdString()));
                                 return;
                             }
                             QString const storageKey = QJsonDocument::fromJson(reply->readAll())
                                                            .object()
                                                            .value(QStringLiteral("storageKey"))
                                                            .toString();
                             if (storageKey.isEmpty()) {
                                 promise->reject(failure("upload failed: the server's reply carried no storage key"));
                                 return;
                             }
                             promise->resolve(client::UploadedBlob{.storageKey = storageKey.toStdString(),
                                                                   .filename = filename,
                                                                   .contentType = contentType,
                                                                   .sizeBytes = size});
                         });
        return std::move(settleable.first);
    }

    [[nodiscard]] morph::async::Completion<std::string> download(client::DownloadRequest request) override {
        auto settleable = morph::async::Completion<std::string>::makeSettleable(_callbacks);
        auto promise = sharedPromise<std::string>(std::move(settleable.second));
        QNetworkRequest get{
            QUrl{_baseUrl + QStringLiteral("/attachments/") + QString::fromStdString(request.storageKey)}};
        get.setRawHeader("Authorization", QByteArray::fromStdString("Bearer " + request.bearerToken));
        QNetworkReply* const reply = _network->get(get);
        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, promise, target = request.localPath] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                promise->reject(failure("download failed: " + reply->errorString().toStdString()));
                return;
            }
            QFile file{QString::fromStdString(target.string())};
            if (!file.open(QIODevice::WriteOnly) || file.write(reply->readAll()) < 0) {
                promise->reject(failure("could not write '" + target.string() + "'"));
                return;
            }
            promise->resolve(target.string());
        });
        return std::move(settleable.first);
    }

private:
    QString _baseUrl;
    morph::exec::IExecutor* _callbacks;
    std::unique_ptr<QNetworkAccessManager> _network;
};

}  // namespace

std::shared_ptr<client::IAttachmentTransfer> makeQtAttachmentTransfer(std::string const& baseUrl,
                                                                     morph::exec::IExecutor& callbacks) {
    return std::make_shared<QtAttachmentTransfer>(baseUrl, callbacks);
}

}  // namespace kanban::qt
```

Create `examples/kanban/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The kanban client: picks the Qt Quick or terminal frontend at runtime (--ui=qt|tui, MORPH_UI, or the first that
// can run here) and hands it the application.
//
//   kanban [--ui=qt|tui] [--server ws://host:port] [--db <odbc>] [--user <name>]
//
// Environment: KANBAN_ATTACHMENT_SERVER names the attachment side channel (default: the server's host, port 8769);
// KANBAN_OFFLINE_QUEUE_DB names the offline move queue (builds with MORPH_BUILD_OFFLINE_SQLITE).

#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <vector>

#include "app/app_environment.hpp"
#include "kanban_application.hpp"

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#endif
#if MORPH_EXAMPLE_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

int main(int argc, char** argv) {
    try {
        auto const env = morph::examples::AppEnvironment::fromArgs(argc, argv);
        std::vector<morph::ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
        built.push_back(morph::qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
        built.push_back(morph::tui::frontendOption());
#endif
        auto const frontend = morph::ui::selectFrontend(built, argc, argv);
        return frontend->run([&env](morph::ui::AppContext& ctx) { return kanban::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "kanban: " << error.what() << '\n';
        return 1;
    }
}
```

Append to `examples/kanban/CMakeLists.txt`:

```cmake
# The Qt Quick frontend's attachment transfer uses Qt's network stack. It is compiled into ladder_kanban_app only
# when that frontend is built; its header is Qt-free, and KANBAN_HAS_QT_TRANSFER (always 0 or 1) tells the
# application whether it may pick it when the running frontend is "qt".
if(TARGET ladder_kanban_app)
    if(TARGET morph_qt_quick)
        find_package(Qt6 6.5 REQUIRED COMPONENTS Network)
        target_sources(ladder_kanban_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/qt/qt_attachment_transfer.cpp")
        target_include_directories(ladder_kanban_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/qt")
        target_link_libraries(ladder_kanban_app PRIVATE Qt6::Network)
        target_compile_definitions(ladder_kanban_app PRIVATE KANBAN_HAS_QT_TRANSFER=1)
    else()
        target_compile_definitions(ladder_kanban_app PRIVATE KANBAN_HAS_QT_TRANSFER=0)
    endif()
endif()
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_smoke_tests kanban
./build/all/examples/kanban/ladder_kanban_smoke_tests
git grep -n -E '#include <Q|morph/qt|morph/tui|morph/qt_quick' -- examples/kanban/app examples/kanban/qt/qt_attachment_transfer.hpp; \
  test $? -eq 1 && echo "app is toolkit-free"
```

Expected: both targets build; 2 smoke cases pass; the check prints "app is toolkit-free".

Mutation check: make `makeApplication` begin with `throw std::runtime_error{"mutation"};`. Expected FAIL in both
smoke cases (the frontend's run reports the factory's failure). Restore it.

By hand, once: `./build/all/examples/kanban/kanban --ui=tui --db "DRIVER=SQLite3;Database=/tmp/kanban-manual.db"` —
sign in, create a project, open it, add two columns, a lane and a task, drag the card with the mouse to the other
column, Ctrl+C. Then the same with `--ui=qt`. Report what you saw in the hand-off; this is the one check of the
mouse path on a real terminal.

- [ ] **Step 5: Commit**

```bash
git add examples/kanban/app/kanban_application.hpp examples/kanban/app/kanban_application.cpp examples/kanban/ui \
        examples/kanban/qt examples/kanban/tests/smoke/test_kanban_frontends.cpp examples/kanban/CMakeLists.txt
git commit -m "wip(kanban): the application, one binary for both frontends, and the Qt attachment transfer

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K14: The headless test client drives the board controller

`tests/test_kanban_process_separation.cpp` spawns real client processes over a real WebSocket and SIGKILLs one.
The client binary stays a Qt-core process (its transport is `QtWebSocketBackend`), but it now runs the same
`BoardController` and `TaskDetailController` the app runs, on a `reactive::Runtime` over a `QtExecutor`, with a
`QTimer`-backed `Scheduler` for the board's polling. Its command line and exit codes do not change, so the
process-separation test does not change. It moves out of `src/headless/` (whose `morph_add_rung` convention needs a
`gui_lib`) into `headless/`, built by the rung's own `CMakeLists.txt`.

**Files:**
- Move: `examples/kanban/src/headless/main.cpp` → `examples/kanban/headless/main.cpp` (then rewrite it)
- Modify: `examples/kanban/CMakeLists.txt` — append the headless block below
- Test: `examples/kanban/tests/test_kanban_process_separation.cpp` (unchanged)

**Interfaces:**
- Consumes: `BoardController`, `TaskDetailController` (Tasks K6, K11); `morph::examples::Wiring` (Part 6);
  `FormSession::assign` (Part 5); `morph::qt::QtExecutor`,
  `morph::qt::QtWebSocketBackend` (`morph/qt/`); `reactive::Scheduler`, `TimerHandle(std::function<void()>)`
  (Part 1).
- Produces: the `ladder_kanban_headless` target and `MORPH_LADDER_HEADLESS_BIN` on `ladder_kanban_tests`.

- [ ] **Step 1: Write the failing check**

The test exists; the failing state is the move:

```bash
git mv examples/kanban/src/headless/main.cpp examples/kanban/headless/main.cpp
cmake build/all && cmake --build build/all --target ladder_kanban_tests
./build/all/examples/kanban/ladder_kanban_tests "[kanban][process]"
```

- [ ] **Step 2: Run it to verify it fails**

Expected: no test case matches `[kanban][process]` (Catch2 reports "No test cases matched"), because
`MORPH_LADDER_HEADLESS_BIN` is no longer defined — `morph_add_rung` finds no `src/headless/*.cpp` — and the whole
file compiles to nothing.

- [ ] **Step 3: Implement**

Replace the whole of `examples/kanban/headless/main.cpp` with:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Headless kanban client, spawned as a separate process by testkit/process_pool.hpp.
//
// Runs the board and task-dialog controllers the real client runs, over a real WebSocket: they are the layer where
// a lost connection has to be survivable (examples/TESTING.md).
//
// Usage: ladder_kanban_headless --url <ws-url> --project <id> [options]
//   --url <ws>       server to connect to (required)
//   --project <id>   project whose board to open (required)
//   --token <tok>    session token; the server's authorizer rejects us without one
//   --principal <p>  principal name to send alongside the token
//   --comments <n>   add n comments to the board's first task, then exit 0
//   --hold           attach, print ATTACHED, then block forever -- the crash test kills an attached client
//
// Exit codes name the step that failed:
//   10 connect failed   11 openBoard failed   12 openBoard timed out
//   13 no task to comment on                  14 comment failed/timed out
//    2 bad arguments

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "app/wiring.hpp"
#include "controllers/board_controller.hpp"
#include "controllers/task_detail_controller.hpp"

namespace {

using namespace std::chrono_literals;

// Timers on the Qt event loop this process runs: the board's polling arms one.
class QtTimerScheduler final : public morph::reactive::Scheduler {
public:
    [[nodiscard]] morph::reactive::TimerHandle after(std::chrono::milliseconds delay,
                                                     std::function<void()> fn) override {
        return arm(delay, std::move(fn), true);
    }
    [[nodiscard]] morph::reactive::TimerHandle every(std::chrono::milliseconds period,
                                                     std::function<void()> fn) override {
        return arm(period, std::move(fn), false);
    }

private:
    [[nodiscard]] static morph::reactive::TimerHandle arm(std::chrono::milliseconds interval, std::function<void()> fn,
                                                          bool singleShot) {
        auto timer = std::make_shared<QTimer>();
        timer->setSingleShot(singleShot);
        QObject::connect(timer.get(), &QTimer::timeout, timer.get(), std::move(fn));
        timer->start(interval);
        return morph::reactive::TimerHandle{[timer] { timer->stop(); }};
    }
};

// Spins this process's event loop until @p done or the budget runs out.
bool pumpUntil(std::function<bool()> const& done, std::chrono::milliseconds budget) {
    auto const deadline = std::chrono::steady_clock::now() + budget;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(5ms);
    }
    return done();
}

struct Options {
    QString url;
    std::string token;
    std::string principal{"alice"};
    std::int64_t projectId = 0;
    int comments = 0;
    bool hold = false;
};

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};

    Options opts;
    for (int idx = 1; idx < argc; ++idx) {
        std::string const arg{argv[idx]};
        auto const next = [&]() -> std::string { return idx + 1 < argc ? argv[++idx] : std::string{}; };
        if (arg == "--url") {
            opts.url = QString::fromStdString(next());
        } else if (arg == "--token") {
            opts.token = next();
        } else if (arg == "--principal") {
            opts.principal = next();
        } else if (arg == "--project") {
            opts.projectId = std::stoll(next());
        } else if (arg == "--comments") {
            opts.comments = std::stoi(next());
        } else if (arg == "--hold") {
            opts.hold = true;
        }
    }
    if (opts.url.isEmpty() || opts.projectId == 0) {
        std::cerr << "usage: ladder_kanban_headless --url <ws> --project <id> [--token t] [--comments n] [--hold]\n";
        return 2;
    }

    auto backend =
        std::make_unique<morph::qt::QtWebSocketBackend>(QUrl{opts.url}, morph::model::detail::defaultDispatcher(),
                                                        morph::model::detail::defaultRegistry(), std::nullopt);
    if (!backend->waitForConnected(5000)) {
        std::cerr << "headless: connect failed\n";
        return 10;
    }
    morph::qt::QtExecutor executor;
    morph::bridge::Bridge bridge{std::move(backend), executor};
    morph::session::Context session;
    session.principal = opts.principal;
    session.token = opts.token;
    bridge.setDefaultSession(session);
    morph::reactive::Runtime runtime{executor};
    QtTimerScheduler scheduler;
    morph::examples::Wiring const wiring{
        .runtime = runtime, .scheduler = scheduler, .bridge = bridge, .callbacks = executor};
    kanban::client::BoardController board{wiring};
    kanban::client::TaskDetailController detail{wiring, board, nullptr};

    board.open(kanban::ProjectId{opts.projectId}, kanban::Role::Member);
    if (!pumpUntil([&board] { return board.board().has_value() || !board.errorText().empty(); }, 10s)) {
        std::cerr << "headless: openBoard timed out\n";
        return 12;
    }
    if (!board.board().has_value()) {
        std::cerr << "headless: openBoard failed: " << board.errorText() << "\n";
        return 11;
    }

    if (opts.hold) {
        // Attached, and staying attached: the parent kills us only after this line, so the kill lands on a client
        // the server holds a live connection scope for.
        std::cout << "ATTACHED\n";
        std::cout.flush();
        for (;;) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            std::this_thread::sleep_for(50ms);
        }
    }

    for (int done = 0; done < opts.comments; ++done) {
        if (!board.board().has_value() || board.board()->tasks.empty()) {
            std::cerr << "headless: --comments given but the board has no task\n";
            return 13;
        }
        detail.openTask(board.board()->tasks.front().id);
        auto& form = detail.addCommentForm();
        form.assign("body", R"("from a separate process")");
        if (!form.ready()) {
            std::cerr << "headless: the comment form is not ready\n";
            return 14;
        }
        form.submit();
        bool const settled = pumpUntil([&form] { return !form.pending(); }, 10s);
        if (!settled || form.lastError() != nullptr || !form.lastReply().has_value()) {
            std::cerr << "headless: addComment failed: "
                      << (settled ? morph::reactive::errorMessage(form.lastError()) : std::string{"timed out"}) << "\n";
            return 14;
        }
    }
    return 0;
}
```

Append to `examples/kanban/CMakeLists.txt`:

```cmake
# The headless test client: a Qt-core process that runs the board controller over a real WebSocket.
# tests/test_kanban_process_separation.cpp spawns it.
if(TARGET ladder_kanban_app AND TARGET morph_qt_impl AND NOT EMSCRIPTEN)
    add_executable(ladder_kanban_headless "${CMAKE_CURRENT_SOURCE_DIR}/headless/main.cpp")
    target_link_libraries(ladder_kanban_headless PRIVATE ladder_kanban_app morph::qt morph_qt_impl Qt6::Core)
    target_compile_features(ladder_kanban_headless PRIVATE cxx_std_23)
    if(COMMAND morph_suppress_test_dialogs)
        morph_suppress_test_dialogs(ladder_kanban_headless)
    endif()
    apply_bigobj(ladder_kanban_headless)
    if(AF_COVERAGE)
        apply_coverage(ladder_kanban_headless TEST)
    endif()
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(ladder_kanban_headless ${AF_SANITIZER})
    endif()
    if(TARGET ladder_kanban_tests)
        target_compile_definitions(ladder_kanban_tests PRIVATE
            MORPH_LADDER_HEADLESS_BIN="$<TARGET_FILE:ladder_kanban_headless>")
        add_dependencies(ladder_kanban_tests ladder_kanban_headless)
    endif()
endif()
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests ladder_kanban_headless
./build/all/examples/kanban/ladder_kanban_tests "[kanban][process]"
```

Expected: PASS, 2 cases ("real client processes drive one shared board" sees 4 comments; "a killed client's
models are reclaimed").

Mutation check: in the headless comment loop, delete `form.submit();`. Expected FAIL in "real client processes
drive one shared board" (every child exits 14). Restore it.

- [ ] **Step 5: Commit**

```bash
git add -A examples/kanban/src/headless examples/kanban/headless examples/kanban/CMakeLists.txt
git commit -m "wip(kanban): the headless test client runs the board controller

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task K15: Retire kanban's QML stack; the README's client section

Every claim the deleted files made now has a controller, view or smoke test (the table below). This task deletes
the QML, the bridges, the presenters and the old desktop main, the tests that existed only for them, and every
pointer to them outside `docs/superpowers/` (dated plans) and `CHANGELOG.md` (published entries do not move).

| Deleted test file | What it pinned | Where that claim lives now |
|---|---|---|
| `test_project_admin_presenter.cpp` | refresh, create, list roles, set/remove a member, login installs the session, overlapping creates, failures routed | `test_projects_controller.cpp` (list, create, members round-trip, refusal in `errorText`); `test_session_controller.cpp` (session installed, token verifies); overlapping per-call results: Part 6's `mapCompletion` test "two overlapping completions each deliver their own value" (`examples/common`; every form submission is one `mapCompletion` with its own closure) |
| `test_project_admin_qml_bridge.cpp` | the same through property bags; `submitIfValid` login redacts the token; a refused foreign action; no raw token on any signal | `test_session_controller.cpp` "the sign-in reply on screen carries no token"; the surface audit and the refusal of foreign actions are gone with the routing table they guarded — each form's submitter serves its own action only |
| `test_board_presenter.cpp` | open reports the empty board; a stalled Socket-mode attach still lands; create column/lane/task; move; comment, activity and events round-trip; failures routed | `test_board_controller.cpp`, `test_board_socket.cpp`, `test_task_detail_controller.cpp` (comment), `test_board_polling.cpp` (events since a cursor) |
| `test_board_qml_bridge.cpp` | board property through forms; a fresh op id per move; comment; forms keep the board live; CreateRule re-lists rules; `fetchOptions` serves `GetBoardState` only; role text; rule round trip; `failed` on a bad project id; the poller applies another client's move; upload/download, 404, cross-project 404, no attachment server | `test_board_controller.cpp` (forms, fresh op id, bad project id, role text), `test_rules_controller.cpp` (rule round trip; the options come from the attached board's handler, and an action it does not serve is refused by name), `test_board_polling.cpp`, `test_task_detail_controller.cpp` (no transfer configured), `test_task_detail_attachments.cpp` and `test_http_attachment_transfer.cpp` (round trip, 404, cross-project 404) |
| `test_board_concurrent_drag.cpp` | N=4 concurrent moves keep positions dense; no shared per-call field | rewritten in place over `BoardController` (Task K7), plus "48 gestures, 48 op ids" |
| `test_board_offline_bridge.cpp` | queue while offline, replay on reconnect; dead-letter count; offline metrics | `test_board_offline.cpp` (Task K8) and `test_offline_moves.cpp` (Task K5) |
| `test_gui_forms_render.cpp` | each form renders through the shipped QML renderer and submits | `test_kanban_views.cpp`: every screen mounted on the `RecordingBackend`, each form's own Submit button clicked; the renderer itself is spec 2's to test |
| `test_board_layout.cpp` | the QML board keeps a usable area and every column reachable | `test_kanban_views.cpp` "the board is a grid…": every column header is a grid cell and the grid sits in a horizontal `Scroll`; pixel layout is each frontend's own (Parts 3 and 4) |
| `test_kanban_qml_surface.cpp` | bridges expose exactly what QML binds | replaced by construction: views call controller members, so a missing one fails to compile |
| `test_gui_qml_smoke.cpp` | Main.qml loads | `tests/smoke/test_kanban_frontends.cpp` (TUI and Qt Quick) |

**Files:**
- Delete: `examples/kanban/gui/` (all), `examples/kanban/gui_lib/` (all), and in `examples/kanban/tests/`:
  `test_board_presenter.cpp`, `test_board_qml_bridge.cpp`, `test_board_offline_bridge.cpp`,
  `test_project_admin_presenter.cpp`, `test_project_admin_qml_bridge.cpp`, `test_gui_forms_render.cpp`,
  `test_board_layout.cpp`, `test_kanban_qml_surface.cpp`, `test_gui_qml_smoke.cpp`
- Modify: `examples/kanban/CMakeLists.txt` (whole file, below), `examples/kanban/README.md`, `codecov.yml`,
  `scripts/coverage.sh`, `docs/spec/offline/offline.md`, `examples/IMPLEMENTATION.md`, and the comments listed in
  Step 3 (in `include/kanban/dto/`, `include/kanban/models/board_model.hpp`, `src/models/board_model.cpp`,
  `tests/test_board_model.cpp`, `tests/test_kanban_offline.cpp`)
- Test: the whole kanban suite, plus the check below

**Interfaces:**
- Consumes: everything above.
- Produces: nothing new.

- [ ] **Step 1: Write the failing check**

```bash
test ! -e examples/kanban/gui && test ! -e examples/kanban/gui_lib && \
  { git grep -n -E 'board_qml_bridge|BoardBridge|ProjectAdminBridge|BoardPresenter|kanban/gui_lib|kanban/gui/|gui/qml/' -- \
      examples/kanban docs/spec examples/IMPLEMENTATION.md codecov.yml; test $? -eq 1; } && \
  echo "kanban's QML stack is gone"
```

- [ ] **Step 2: Run it to verify it fails**

Expected: no "gone" line (the directories exist and the grep lists `README.md`, `docs/spec/offline/offline.md:806`,
`examples/IMPLEMENTATION.md:62`, `codecov.yml:642`).

- [ ] **Step 3: Implement**

```bash
git rm -r -q examples/kanban/gui examples/kanban/gui_lib
git rm -q examples/kanban/tests/test_board_presenter.cpp examples/kanban/tests/test_board_qml_bridge.cpp \
          examples/kanban/tests/test_board_offline_bridge.cpp examples/kanban/tests/test_project_admin_presenter.cpp \
          examples/kanban/tests/test_project_admin_qml_bridge.cpp examples/kanban/tests/test_gui_forms_render.cpp \
          examples/kanban/tests/test_board_layout.cpp examples/kanban/tests/test_kanban_qml_surface.cpp \
          examples/kanban/tests/test_gui_qml_smoke.cpp
```

Replace the whole of `examples/kanban/CMakeLists.txt` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# kanban — rung 4 of the application ladder (examples/kanban/README.md).
# morph_add_rung() wires the standard targets: the domain library, the client app library (app/), the kanban
# binary (ui/main.cpp), the server and the tests. This file adds what is particular to this rung.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME kanban)

# morph_add_rung() globs src/models, src/db and src/app into ladder_kanban_lib; the authorizer and Login's
# validation live beside them, in src/auth and src/dto.
if(TARGET ladder_kanban_lib)
    target_sources(ladder_kanban_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/kanban_authorizer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# The attachment side channel (src/server/app/attachment_server.cpp) is a QTcpServer. It lives in
# ladder_kanban_server_app, which only the server and the tests link, so the domain library stays toolkit-free.
if(TARGET ladder_kanban_server_app)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Network)
    target_link_libraries(ladder_kanban_server_app PUBLIC Qt6::Network)
endif()

# The durable offline move queue (app/controllers/offline_moves.cpp) is optional, like every
# morph::offline_sqlite consumer: without MORPH_BUILD_OFFLINE_SQLITE the board sends every move.
if(MORPH_BUILD_OFFLINE_SQLITE)
    if(TARGET ladder_kanban_app)
        target_compile_definitions(ladder_kanban_app PUBLIC MORPH_BUILD_OFFLINE_SQLITE)
        target_link_libraries(ladder_kanban_app PUBLIC morph::offline_sqlite)
    endif()
    if(TARGET ladder_kanban_tests)
        target_compile_definitions(ladder_kanban_tests PRIVATE MORPH_BUILD_OFFLINE_SQLITE)
        target_link_libraries(ladder_kanban_tests PRIVATE morph::offline_sqlite)
    endif()
endif()

# The Qt Quick frontend's attachment transfer uses Qt's network stack. It is compiled into ladder_kanban_app only
# when that frontend is built; its header is Qt-free, and KANBAN_HAS_QT_TRANSFER (always 0 or 1) tells the
# application whether it may pick it when the running frontend is "qt".
if(TARGET ladder_kanban_app)
    if(TARGET morph_qt_quick)
        find_package(Qt6 6.5 REQUIRED COMPONENTS Network)
        target_sources(ladder_kanban_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/qt/qt_attachment_transfer.cpp")
        target_include_directories(ladder_kanban_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/qt")
        target_link_libraries(ladder_kanban_app PRIVATE Qt6::Network)
        target_compile_definitions(ladder_kanban_app PRIVATE KANBAN_HAS_QT_TRANSFER=1)
    else()
        target_compile_definitions(ladder_kanban_app PRIVATE KANBAN_HAS_QT_TRANSFER=0)
    endif()
endif()

# The headless test client: a Qt-core process that runs the board controller over a real WebSocket.
# tests/test_kanban_process_separation.cpp spawns it.
if(TARGET ladder_kanban_app AND TARGET morph_qt_impl AND NOT EMSCRIPTEN)
    add_executable(ladder_kanban_headless "${CMAKE_CURRENT_SOURCE_DIR}/headless/main.cpp")
    target_link_libraries(ladder_kanban_headless PRIVATE ladder_kanban_app morph::qt morph_qt_impl Qt6::Core)
    target_compile_features(ladder_kanban_headless PRIVATE cxx_std_23)
    if(COMMAND morph_suppress_test_dialogs)
        morph_suppress_test_dialogs(ladder_kanban_headless)
    endif()
    apply_bigobj(ladder_kanban_headless)
    if(AF_COVERAGE)
        apply_coverage(ladder_kanban_headless TEST)
    endif()
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(ladder_kanban_headless ${AF_SANITIZER})
    endif()
    if(TARGET ladder_kanban_tests)
        target_compile_definitions(ladder_kanban_tests PRIVATE
            MORPH_LADDER_HEADLESS_BIN="$<TARGET_FILE:ladder_kanban_headless>")
        add_dependencies(ladder_kanban_tests ladder_kanban_headless)
    endif()
endif()
```

`examples/kanban/README.md`:

1. Insert this section immediately before `## Expected strain points`:

```markdown
## The client

One binary, `kanban` (`ui/main.cpp`), runs on Qt Quick or in a terminal: `--ui=qt|tui`, else `MORPH_UI`, else the
first frontend that can run here. `--server ws://host:port` connects to `ladder_kanban_server`, whose attachment
side channel is `KANBAN_ATTACHMENT_SERVER` when set, else port 8769 on the server's host; without `--server` the app
runs locally against `--db` (default `kanban.db`), signs its own tokens and has no attachments. Against a server, in
a build with `MORPH_BUILD_OFFLINE_SQLITE`, moves made while the server is unreachable wait in the queue
`KANBAN_OFFLINE_QUEUE_DB` names (default `kanban-offline-queue.db`).

`app/` is `ladder_kanban_app`, which links no toolkit:

- `app/controllers/`: `SessionController` (sign-in; the token reaches the bridge, never the screen),
  `ProjectsController` (the list, create, members and the role picker), `BoardController` (attach, the board and
  activity queries, the board forms, moves under a fresh op id, polling, the offline hook), `RulesController`,
  `TaskDetailController` (comments, and attachments through an injected `IAttachmentTransfer`) and `OfflineMoves`
  (the durable queue, its network monitor and the reconnect replay).
- `app/views/`: one `ui::Node` builder per screen, bindings only.
- `app/http/`: the terminal's attachment transfer, a minimal HTTP/1.1 client over POSIX sockets. On Qt Quick,
  `qt/qt_attachment_transfer.cpp` (`QNetworkAccessManager`, compiled into the app library only when Qt Quick is
  built, behind a Qt-free header) moves the bytes instead.

`headless/main.cpp` is the process-separation test's client: a Qt-core process running the same `BoardController`
over `QtWebSocketBackend`.

**What looks different from the old Qt Quick screens.** The behaviour is kept; the styling is not reproduced:

- The board has no custom styling — no fixed-width bordered columns, no card rectangles, no blue or red drop
  highlight at the WIP limit (a drop into a full column is refused by the server, and the error line says so), no
  always-on scrollbar. It is a grid of lanes by columns in a horizontal scroll.
- A card dropped on another card lands before it; dropped on a cell, it lands at the end. The old board computed the
  position from the drop's pixel offset.
- A task is created from the "+ task" button in its cell, which opens a dialog, instead of a form inline in every
  cell; a project opens with its "Open" button instead of a click on the row.
- A form's outcome shows under the form (the forms engine's reply line); the screen's own status line shows errors.
- In a terminal a file is chosen by typing its path. On Windows the terminal client has no attachments and no
  `--server`, because `morph::net` and the HTTP client are POSIX-only; Qt Quick on Windows has both.
```

2. In "morph subsystems exercised": in the forms table, replace the Screen column's `gui/qml/…` paths with
   `app/views/sign_in_view.cpp` (Sign in), `app/views/projects_view.cpp` (Create project; Add member, "members
   panel"), `app/views/board_view.cpp` (Add column; Add swimlane; New task, "a dialog from each cell's + task";
   Add comment, "task dialog"; Add rule, "rules dialog"). Replace the paragraph that begins "Not one of them has a
   hand-written field" with: "Not one of them has a hand-written field: each is a runtime form read from
   `schemaJson<A>()` (`FormModel::forAction<A>()`) and rendered by `formView`, and if any of those eight actions
   grows a member, no view changes. Each form submits through the one handler that serves its action, so there is
   no routing table to get wrong." In the next paragraph replace "the view that owns the form supplies them with
   `setFieldValue`" with "the controller that owns the form prefills them", and "the renderer fetches its own
   options for via `BoardBridge::fetchOptions`" with "whose options `RulesController::fetchOptions` serves from the
   attached board". Replace the paragraph that begins "**This is rendered, not merely generated.**" with:
   "`tests/client/test_kanban_views.cpp` mounts every screen on the `RecordingBackend` and clicks each form's own
   Submit button; the controller tests pin what each submission does."
3. In "What is still hand-built, and why": the two table rows become `` `app/views/board_view.cpp`'s drag-and-drop
   board `` and `` `app/views/projects_view.cpp`'s per-row role `Select` `` (the justification column unchanged
   apart from "on selection change" → "on selection" and "the create-a-new-thing shape `DynamicForm` renders" →
   "the create-a-new-thing shape a form renders"). In the paragraph beginning "**There is no enum-rendering
   gap.**" replace "`DynamicForm` draws that as a combo box" with "the forms engine draws that as a `Select`", the
   `gui/qml/MembersView.qml`/`gui/qml/RulesView.qml` names with "the members panel's add-member form and the rules
   dialog's add-rule form", "are both `DynamicForm`s now" with "are both runtime forms", and "through `BoardBridge::fetchOptions`. Only `MembersView`'s **per-row** role picker"
   with "through `RulesController::fetchOptions`. Only the members panel's **per-row** role picker". In the
   drag-and-drop paragraph replace "(`DropArea.onDropped` computes them from the drop's own geometry)" with "(the card
   or cell the drop lands on)".
4. In "Expected strain points" item 7's last sentence replace "`BoardBridge`'s `DeadLetterSink` turns that into a
   `syncStatusChanged(queueDepth, deadLettered)` emission the GUI renders" with "`OfflineMoves`' dead-letter sink
   turns that into the board's \"N changes could not be synced\" line"; in the queue-bound bullet replace
   "`BoardBridge::enableOfflineQueue` constructs its" with "`OfflineMoves` (`app/controllers/offline_moves.cpp`)
   constructs its" and "inside a `Q_INVOKABLE` move handler that has no failure path today" with "inside
   `BoardController::moveTask`, which has no user-visible failure path today".
5. In "Findings", replace "`MembersView`/`RulesView`'s add-member/add-rule forms" with "the add-member and
   add-rule forms".
6. In "Definition of done", replace "(`test_board_offline_bridge.cpp`)" with "(`tests/client/test_board_offline.cpp`)".

`codecov.yml`: replace the three kanban lines (`examples/kanban/tests/**`, `examples/kanban/gui/**`,
`examples/kanban/gui_wasm/**`) with

```yaml
  # kanban's client shells: ui/ is main(), qt/ the Qt Quick-only transfer (exercised by hand), headless/ a
  # spawned test process. app/ is measured.
  - "examples/kanban/tests/**"
  - "examples/kanban/ui/**"
  - "examples/kanban/qt/**"
  - "examples/kanban/headless/**"
```

`scripts/coverage.sh`: if the rung loop (`for _sub in include src gui_lib; do`) does not already name `app`, make
it `for _sub in include src app gui_lib; do`, and in the comment above it replace "gui_lib/ is its hand-written
presenter/adapter code" with "app/ (and, until the last rung moves, gui_lib/) is its hand-written client code".

`docs/spec/offline/offline.md`: replace the bullet that begins "- `examples/kanban/gui_lib/board_qml_bridge.cpp` —
the transport-shaped half" (six lines, ending "belongs in a `FieldOutbox`-shaped class instead.") with:

```markdown
- `examples/kanban/app/controllers/offline_moves.cpp` — the transport-shaped half only: probe, serialise, enqueue
  under the move's op id, replay through the board's handler, report queue depth. Nothing in it is domain-shaped,
  so it stands as client glue; anything with a domain invariant in it belongs in a `FieldOutbox`-shaped class
  instead.
```

`examples/IMPLEMENTATION.md`: replace "kanban's copy in a QML bridge (`examples/kanban/gui_lib/board_qml_bridge.cpp`)
stands only because it is transport-shaped glue under rule 2's glue justification." with "kanban's copy
(`examples/kanban/app/controllers/offline_moves.cpp`) stands only because it is transport-shaped glue with no domain
invariant in it."

Comments that name the deleted client (each states what the code does now; replace exactly the quoted text):

- `include/kanban/dto/board_dto.hpp` (`CreateTask`): "the board view supplies both from the delegate that owns the
  form (`gui/qml/BoardView.qml`)." → "the board controller prefills both when a cell's \"+ task\" dialog opens."
  (`AddComment`): "whichever task's detail popup is open, and `gui/qml/TaskDetailPopup.qml` supplies it." →
  "whichever task's detail dialog is open, and `TaskDetailController` prefills it."
- `include/kanban/dto/project_dto.hpp`: "Carries over the placeholder the hand-built `TextField` this form replaced
  used to show (`gui/qml/ProjectListView.qml`)." → "The label and placeholder the projects screen's create form
  shows."; "and `gui/qml/MembersView.qml` supplies it from the page the form is embedded in." → "and the projects
  controller prefills it for the open members panel."
- `include/kanban/dto/rule_dto.hpp`: "and `gui/qml/RulesView.qml` supplies it from the attached `BoardBridge`." →
  "and `RulesController` prefills it from the attached board."; "(no presenter/QML bridge ever constructs one
  directly)" → "(no client controller ever constructs one directly)".
- `include/kanban/models/board_model.hpp`: "(test files, the QML/GUI bridge, app.cpp)" → "(test files, the client
  app library, app.cpp)"; "which is what makes `BoardBridge::openBoard("not-a-number")` (parsed into a
  default-constructed `ProjectId{}` by board_qml_bridge.cpp's `parseId`) a rejected `Completion`" → "which is what
  makes an `OpenBoard` carrying a default-constructed `ProjectId{}` a rejected `Completion`"; "(`morph::ladder::gui::
  errorText` reads `what()` off any `std::exception`)" → "(the client's `reactive::errorMessage` reads `what()` off
  any `std::exception`)". `src/models/board_model.cpp`: "(kanban's tests, the QML/GUI bridge, app.cpp)" →
  "(kanban's tests, the client app library, app.cpp)".
- `tests/test_board_model.cpp`: the four lines "Regression test for the QML TaskDetailPopup gap: … looked identical
  once serialized." → "Each comment names its task, so a client can show one task's comments from a board with
  comments on several."; "What TaskDetailPopup.qml's own filter now expresses client-side:" → "What the task detail
  dialog's filter expresses client-side:"; the paragraph "A regression test for a real crash: BoardBridge::openBoard()
  parses … projectId\") expects." →
  "An `OpenBoard` with a disengaged `projectId` (what a client makes of an id it cannot parse) reaches
  `BridgeHandler::execute()` before `BoardModel::execute(OpenBoard)`'s own `hasValue()` check runs, because
  `ActionKeyTraits<OpenBoard>::key()` computes the routing key first. Dereferencing the empty optional there is
  undefined behaviour, which libstdc++'s hardened build turns into an abort of the whole process; the key must
  refuse it instead."; and "`BoardBridge` routes it through `morph::ladder::gui::errorText()`, which reads" → "the
  client shows it through `reactive::errorMessage`, which reads".
- `tests/test_kanban_offline.cpp`: "a real SqliteOfflineQueue-backed presenter would replay through" → "the client's
  `OfflineMoves` replays through"; the paragraph "This scenario is driven at the backend level -- … multi-client-online
  cases respectively." →
  "Driven at the backend level -- two in-process `BoardModel` handles standing in for two clients' offline queues --
  because `SyncWorker::run()` drains a whole queue in one call, so a client's own `OfflineMoves` offers no seam to
  alternate one item from each queue. Each queue is a plain `std::vector<MoveTaskPosition>` (what
  `SqliteOfflineQueue` persists while offline), replayed through `BoardModel::execute()` as the reconnect test above
  does for one client. That still exercises the concurrency-sensitive code (the per-project strand and
  `MoveTaskPosition`'s renumbering and ledger); the client plumbing around it is `tests/client/test_board_offline.cpp`'s
  and `test_board_concurrent_drag.cpp`'s."; and "mirroring two distinct BoardBridge/SyncWorker instances" →
  "mirroring two distinct clients' `OfflineMoves`".

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_kanban_tests ladder_kanban_server kanban ladder_kanban_headless
./build/all/examples/kanban/ladder_kanban_tests
```

Then the check from Step 1. Expected: every kanban case passes, the server, binary and headless client build, and
the check prints "kanban's QML stack is gone".

Mutation check: restore `examples/kanban/tests/test_gui_qml_smoke.cpp` from `HEAD`
(`git checkout HEAD -- examples/kanban/tests/test_gui_qml_smoke.cpp`) and rebuild. Expected: the build fails
(`MORPH_LADDER_QML_URI` and the `ladder_kanban_qml` module no longer exist) — proof that the deleted QML stack is not
still being built somewhere. Delete it again.

- [ ] **Step 5: Commit**

```bash
git add -A examples/kanban codecov.yml scripts/coverage.sh docs/spec/offline/offline.md examples/IMPLEMENTATION.md
git commit -m "wip(kanban): retire kanban's QML, bridges and presenters; README client section

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task K16: Kanban group verification and squash

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the full ladder suite**

```bash
cmake --build build/all && ctest --test-dir build/all -L ladder-kanban --output-on-failure
ctest --test-dir build/all -L ladder --output-on-failure
```

Expected: every test passes, kanban's and every other rung's.

- [ ] **Step 2: No frontend, no offline queue**

```bash
cmake --build build/nofront --target ladder_kanban_tests ladder_kanban_app
./build/nofront/examples/kanban/ladder_kanban_tests "[kanban]"
```

Expected: builds (the `#else` of the offline hook compiles); every case passes. The smoke binary, if this
configure builds it, skips both cases (`runFrontendSmoke` skips a frontend the configure did not build).
`cmake --build build/nofront --target kanban` reports no such target — the binary needs a frontend (spec 4 §6).

- [ ] **Step 3: Toolkit-free and UI-free checks**

```bash
git grep -n -E '#include <Q|morph/qt|morph/tui|morph/qt_quick' -- examples/kanban/app; test $? -eq 1 && echo ok-app
git grep -n 'morph/ui/' -- examples/kanban/app/controllers; test $? -eq 1 && echo ok-controllers
git grep -n -E '#include <Q' -- examples/kanban/include examples/kanban/src ':!examples/kanban/src/server'; \
  test $? -eq 1 && echo ok-domain
```

Expected: `ok-app`, `ok-controllers`, `ok-domain`.

- [ ] **Step 4: Sanitizers** (Linux; on macOS an ASan configure of the same tree)

```bash
cmake --preset clang-tsan -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=kanban -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_OFFLINE_SQLITE=ON
cmake --build --preset clang-tsan --target ladder_kanban_tests
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/examples/kanban/ladder_kanban_tests tsan
./build/clang-tsan/examples/kanban/ladder_kanban_tests "[kanban][client]"
```

Expected: clean. The concurrent-drag case, `OfflineMoves`' three threads and the HTTP transfer's worker are what
TSan is here for; the instrumentation check is what makes a clean run mean something.

- [ ] **Step 5: clang-tidy over the changed lines** — CONTRIBUTING's recipe with `origin/master...HEAD` and the file
  count asserted non-zero, configured with `-DMORPH_BUILD_LADDER=ON -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON
  -DMORPH_BUILD_OFFLINE_SQLITE=ON`. Expected: no findings in `examples/kanban/`.

- [ ] **Step 6: Commit any fixes**

```bash
git add -A examples/kanban
git commit -m "wip(kanban): fixes from the sanitizer and tidy gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

- [ ] **Step 7: Squash the group into its one commit**

Follow the master plan's "Squashing a part" procedure with key `kanban` and this message:

```text
examples/kanban: one app, any frontend

The kanban client becomes a toolkit-free app library (ladder_kanban_app):
session, projects, board, rules and task-detail controllers built from
Query, Mutation and runtime forms, and views that are bindings only. The
board is a grid of swimlanes by columns whose cards move by drag-and-drop on
both frontends, each move under a fresh op id; polling, the optional offline
queue and the attachment transfer (Qt's network stack or an HTTP/1.1 client
over POSIX sockets) live behind the controllers. One binary picks Qt Quick or
the TUI at runtime; the headless test client runs the same board controller.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure must show `examples/kanban: one app, any frontend` directly after
`examples/polls: …`.

---
# Group `ledger`

### Task L1: The domain library loses Qt — the report runner moves to the server

`ledger::app::App` is a `QObject` with a `QTimer` that sweeps pending report jobs. It runs only in
`ladder_ledger_server` (and in the tests that drive it), so it moves to the server side (spec 4 §2: "Those classes
run only in the server, so they move into `src/server/` and the server target"), using Part 8's convention:
`src/server/app/*.cpp` with headers under `src/server/include/` become `ladder_ledger_server_app`, which the server
and `ladder_ledger_tests` link. The header keeps its include spelling, `"ledger/app/app.hpp"`, so no includer
changes. It still includes every model header, which is what pulls each model's object out of the static library
into the server binary.

**Files:**
- Move: `examples/ledger/include/ledger/app/app.hpp` → `examples/ledger/src/server/include/ledger/app/app.hpp`
- Move: `examples/ledger/src/app/app.cpp` → `examples/ledger/src/server/app/app.cpp`
- Modify: `examples/ledger/include/ledger/core/types.hpp:112` — the path in `kReportRunnerPrincipal`'s comment
- Test: `examples/ledger/tests/test_app.cpp` (unchanged cases), plus the include check below

**Interfaces:**
- Consumes: Part 8's `morph_add_rung` rule `src/server/app/*.cpp` + `src/server/include/` → `ladder_<rung>_server_app`
  (linked by `ladder_<rung>_server` and `ladder_<rung>_tests`), and its Qt-free `ladder_<rung>_lib` once the rung has
  `app/` (Task L2).
- Produces: `ledger::app::App`, unchanged, in `ladder_ledger_server_app`, still included as `"ledger/app/app.hpp"`
  (Task L6's tests use it).

- [ ] **Step 1: Write the failing check**

```bash
git grep -n -E '#include <Q' -- examples/ledger/include examples/ledger/src ':!examples/ledger/src/server'
test $? -eq 1 && echo "domain library is Qt-free"
```

- [ ] **Step 2: Run it to verify it fails**

Expected: it lists `examples/ledger/include/ledger/app/app.hpp:4:#include <QObject>` and `:5:#include <QTimer>`, and
does not print "domain library is Qt-free".

- [ ] **Step 3: Implement**

```bash
mkdir -p examples/ledger/src/server/include/ledger/app examples/ledger/src/server/app
git mv examples/ledger/include/ledger/app/app.hpp examples/ledger/src/server/include/ledger/app/app.hpp
git mv examples/ledger/src/app/app.cpp examples/ledger/src/server/app/app.cpp
```

In `examples/ledger/include/ledger/core/types.hpp`, replace `(\`examples/ledger/include/ledger/app/app.hpp\`)` with
`(\`examples/ledger/src/server/include/ledger/app/app.hpp\`)`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_tests ladder_ledger_server
./build/all/examples/ledger/ladder_ledger_tests "[ledger][app]"
git grep -n -E '#include <Q' -- examples/ledger/include examples/ledger/src ':!examples/ledger/src/server'; \
  test $? -eq 1 && echo "domain library is Qt-free"
cmake --build build/all --target help | grep -c 'ladder_ledger_server_app'
```

Expected: both targets build; the 7 `[ledger][app]` cases pass; the check prints "domain library is Qt-free"; the
last count is non-zero.

Mutation check: `git mv examples/ledger/src/server/app/app.cpp examples/ledger/src/server/app/app.cpp.off`, re-run
`cmake build/all` and build `ladder_ledger_tests`. Expected: it fails to link (`ledger::app::App::App(...)`
undefined). Move it back.

- [ ] **Step 5: Commit**

```bash
git add -A examples/ledger/include/ledger/app examples/ledger/src examples/ledger/include/ledger/core/types.hpp
git commit -m "wip(ledger): the report runner moves to the server; the domain library is Qt-free

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task L2: The ledger app library — display strings and `BootController`

Ledger has no sign-in screen: its GUI ran as a dev principal (`gui/main.cpp`'s `kDevPrincipal`), installed with
`setDefaultSession` locally and through `Login` against a server. `BootController` does the same with `--user`
(spec 4 §3), and with `--seed` creates a "Personal" book for that principal and opens it; otherwise it opens the book
its options name — book 1, the id `Main.qml` hard-coded, in the application (Task L9). Controllers are built from
Part 6's `morph::examples::Wiring`; `morph_add_rung` builds `app/` into `ladder_ledger_app` and links it into
`ladder_ledger_tests`.

**Files:**
- Create: `examples/ledger/app/support/ledger_format.hpp`, `examples/ledger/app/support/ledger_format.cpp`
- Create: `examples/ledger/app/controllers/boot_controller.hpp`, `examples/ledger/app/controllers/boot_controller.cpp`
- Create: `examples/ledger/tests/client/ledger_client_support.hpp`
- Test: `examples/ledger/tests/client/test_ledger_format.cpp`, `examples/ledger/tests/client/test_boot_controller.cpp`

**Interfaces:**
- Consumes: `ledger::formatMoney` (`ledger/core/money.hpp:155`), `currencyToCode`/`codeToCurrency`
  (`ledger/core/units.hpp:37`, `:61`), `ledger::auth::setTokenIssuer` (`ledger/auth/ledger_authorizer.hpp:169`),
  `ledger::AuthModel`, `ledger::LedgerModel`, `CreateLedger`/`CreateLedgerResult` (`ledger/dto/account_dto.hpp:47`,
  `:62`); `morph::examples::Wiring` (Part 6, `"app/wiring.hpp"`).
- Produces:
  - `AccountRow{id, name, kind, currency, balanceText}`, `EntryRow{id, idText, description, dateText}`,
    `StatementLine{currency, countText, amountText}`; `kindText(AccountKind)`, `kindFromText(std::string_view)`,
    `kindNames()`, `parsePositive(std::string_view)`, `parseAtLeast(std::string_view, std::int64_t minimum)`,
    `isMonth(std::string_view)`, `accountRow(AccountInfo const&)`, `entryRow(TransactionEntryInfo const&)`,
    `statementLines(std::string_view) -> std::optional<std::vector<StatementLine>>`, `localOffsetMinutes()`.
  - `BootState{SigningIn, Ready, Failed}`, `BootOptions{principal, remote, seed, ledger}`,
    `BootController(examples::Wiring, BootOptions)`: `state()`, `ledger() -> std::optional<LedgerId>`, `statusText()`.
  - Test support (`ledger::testing`): `LocalClient`, `QtClient`, `await`, `IssuerScope`, `seedLedger(name)`,
    `openAccountDirect(LedgerId, name, AccountKind) -> AccountId`, `currentMonth()`.

- [ ] **Step 1: Write the failing tests**

Create `examples/ledger/tests/client/ledger_client_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "app/wiring.hpp"
#include "ledger/auth/ledger_authorizer.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/pump.hpp"
#include "testkit/wait.hpp"

namespace ledger::testing {

using namespace std::chrono_literals;

inline constexpr std::string_view kSecret = "ledger-client-test-secret-32-bytes!!";

/// Installs the issuer ledger's `Login` mints from, for one test.
class IssuerScope {
public:
    IssuerScope() {
        ledger::auth::setTokenIssuer(
            std::make_shared<morph::session::TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256));
    }
    ~IssuerScope() { ledger::auth::setTokenIssuer(nullptr); }
    IssuerScope(IssuerScope const&) = delete;
    IssuerScope& operator=(IssuerScope const&) = delete;
    IssuerScope(IssuerScope&&) = delete;
    IssuerScope& operator=(IssuerScope&&) = delete;
};

/// A client over a local backend on a MainThreadExecutor. Declare it before the controllers under test.
class LocalClient {
public:
    LocalClient() = default;
    explicit LocalClient(std::string principal) {
        morph::session::Context session;
        session.principal = std::move(principal);
        bridge.setDefaultSession(std::move(session));
    }
    ~LocalClient() = default;
    LocalClient(LocalClient const&) = delete;
    LocalClient& operator=(LocalClient const&) = delete;
    LocalClient(LocalClient&&) = delete;
    LocalClient& operator=(LocalClient&&) = delete;

    template <typename Pred>
    [[nodiscard]] bool settle(Pred done, std::chrono::milliseconds budget = 5000ms) {
        return morph::examples::testing::pumpUntil(owner, std::move(done), budget);
    }
    [[nodiscard]] morph::examples::Wiring wiring() {
        return morph::examples::Wiring{
            .runtime = runtime, .scheduler = scheduler, .bridge = bridge, .callbacks = owner};
    }

    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::reactive::Runtime runtime{owner};
    morph::reactive::testing::ManualScheduler scheduler;
};

/// A client whose owner is the Qt event loop, for tests that also run the Qt-hosted report runner.
class QtClient {
public:
    explicit QtClient(std::string principal) {
        morph::session::Context session;
        session.principal = std::move(principal);
        bridge.setDefaultSession(std::move(session));
    }
    ~QtClient() = default;
    QtClient(QtClient const&) = delete;
    QtClient& operator=(QtClient const&) = delete;
    QtClient(QtClient&&) = delete;
    QtClient& operator=(QtClient&&) = delete;

    template <typename Pred>
    [[nodiscard]] bool settle(Pred done, std::chrono::milliseconds budget = 5000ms) {
        return morph::ladder::testkit::pumpUntil(std::move(done), budget);
    }
    [[nodiscard]] morph::examples::Wiring wiring() {
        return morph::examples::Wiring{
            .runtime = runtime, .scheduler = scheduler, .bridge = bridge, .callbacks = owner};
    }

    morph::exec::ThreadPoolExecutor pool{4};
    morph::qt::QtExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::reactive::Runtime runtime{owner};
    morph::reactive::testing::ManualScheduler scheduler;
};

/// Waits for @p completion on @p client's owner and returns its value, or rethrows its failure.
template <typename Client, typename T>
[[nodiscard]] T await(Client& client, morph::async::Completion<T> completion) {
    struct State {
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto state = std::make_shared<State>();
    completion.thenDetached([state](T const& value) { state->value = value; })
        .onErrorDetached([state](std::exception_ptr error) { state->error = std::move(error); });
    if (!client.settle([&state] { return state->value.has_value() || state->error != nullptr; })) {
        throw std::runtime_error{"await: the completion did not settle"};
    }
    if (state->error != nullptr) {
        std::rethrow_exception(state->error);
    }
    return std::move(*state->value);
}

/// A book with no owner, as the model tests seed one.
[[nodiscard]] inline ledger::LedgerId seedLedger(std::string name) {
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord row;
    row.name = Lightweight::SqlAnsiString<128>{std::move(name)};
    mapper.Create(row);
    return ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};
}

/// Opens an account straight through the model, as "alice".
[[nodiscard]] inline ledger::AccountId openAccountDirect(ledger::LedgerId ledgerId, std::string name,
                                                         ledger::AccountKind kind) {
    morph::session::Context context;
    context.principal = "alice";
    morph::session::detail::ScopedContext const scope{context};
    ledger::LedgerModel model;
    return model
        .execute(ledger::OpenAccount{
            .ledgerId = ledgerId, .name = std::move(name), .kind = kind, .currency = ledger::Currency::USD})
        .id;
}

/// This month as `YYYY-MM`, in UTC (what `Timestamp::now()` dates a transaction with).
[[nodiscard]] inline std::string currentMonth() {
    std::chrono::year_month_day const today{std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    return std::format("{:04}-{:02}", static_cast<int>(today.year()), static_cast<unsigned>(today.month()));
}

}  // namespace ledger::testing
```

Create `examples/ledger/tests/client/test_ledger_format.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>

#include "support/ledger_format.hpp"

using ledger::AccountKind;

TEST_CASE("ledger::client: account kinds round-trip through their display names", "[ledger][client]") {
    for (AccountKind const kind : {AccountKind::Asset, AccountKind::Expense, AccountKind::Revenue, AccountKind::Liability}) {
        CHECK(ledger::client::kindFromText(ledger::client::kindText(kind)) == kind);
    }
    CHECK(ledger::client::kindFromText("nonsense") == AccountKind::Asset);
    CHECK(ledger::client::kindNames().size() == 4);
}

TEST_CASE("ledger::client: ids and amounts parse whole and in range, or not at all", "[ledger][client]") {
    CHECK(ledger::client::parsePositive("12") == std::optional<std::int64_t>{12});
    CHECK_FALSE(ledger::client::parsePositive("0").has_value());
    CHECK_FALSE(ledger::client::parsePositive("-3").has_value());
    CHECK_FALSE(ledger::client::parsePositive("12x").has_value());
    CHECK_FALSE(ledger::client::parsePositive("").has_value());
    CHECK(ledger::client::parseAtLeast("0", 0) == std::optional<std::int64_t>{0});
}

TEST_CASE("ledger::client::isMonth accepts YYYY-MM only", "[ledger][client]") {
    CHECK(ledger::client::isMonth("2026-01"));
    CHECK_FALSE(ledger::client::isMonth("2026-13"));
    CHECK_FALSE(ledger::client::isMonth("2026/01"));
    CHECK_FALSE(ledger::client::isMonth("26-01"));
}

TEST_CASE("ledger::client::statementLines renders a report body exactly, and refuses garbage", "[ledger][client]") {
    auto const lines = ledger::client::statementLines(
        R"([{"currency":"USD","numerator":-5000,"denominator":1,"decimalPlaces":2,"transactionCount":1}])");
    REQUIRE(lines.has_value());
    REQUIRE(lines->size() == 1);
    CHECK(lines->front() == ledger::client::StatementLine{.currency = "USD", .countText = "1 transactions", .amountText = "-50"});
    CHECK_FALSE(ledger::client::statementLines("not json").has_value());
}

TEST_CASE("ledger::client::localOffsetMinutes is a real UTC offset", "[ledger][client]") {
    int const offset = ledger::client::localOffsetMinutes();
    CHECK(offset >= -14 * 60);
    CHECK(offset <= 14 * 60);
}
```

Create `examples/ledger/tests/client/test_boot_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <optional>

#include "controllers/boot_controller.hpp"
#include "ledger_client_support.hpp"
#include "testkit/db_fixture.hpp"

using ledger::client::BootController;
using ledger::client::BootOptions;
using ledger::client::BootState;
using ledger::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

TEST_CASE("ledger::client::BootController: local mode installs --user and opens the configured book",
          "[ledger][client]") {
    DbFixture const fixture;
    LocalClient client;
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = 7}};

    CHECK(boot.state() == BootState::Ready);
    CHECK(boot.ledger() == std::optional<ledger::LedgerId>{ledger::LedgerId{7}});
    CHECK(client.bridge.defaultSession().principal == "alice");
}

TEST_CASE("ledger::client::BootController: --seed creates a book for the principal and opens it", "[ledger][client]") {
    DbFixture const fixture;
    LocalClient client;
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = false, .seed = true, .ledger = 1}};
    CHECK(boot.state() == BootState::SigningIn);

    REQUIRE(client.settle([&boot] { return boot.state() != BootState::SigningIn; }));

    REQUIRE(boot.state() == BootState::Ready);
    REQUIRE(boot.ledger().has_value());
    morph::bridge::BridgeHandler<ledger::LedgerModel, morph::bridge::AllowShared> handler{client.bridge, &client.owner};
    auto const state = ledger::testing::await(client, handler.execute(ledger::GetLedger{.ledgerId = *boot.ledger()}));
    CHECK(state.accounts.empty());
}

TEST_CASE("ledger::client::BootController: remote mode signs in through Login and installs the token",
          "[ledger][client]") {
    DbFixture const fixture;
    ledger::testing::IssuerScope const issuer;
    LocalClient client;
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = true, .seed = false, .ledger = 1}};

    REQUIRE(client.settle([&boot] { return boot.state() != BootState::SigningIn; }));

    CHECK(boot.state() == BootState::Ready);
    CHECK(client.bridge.defaultSession().principal == "alice");
    CHECK_FALSE(client.bridge.defaultSession().token.empty());
}

TEST_CASE("ledger::client::BootController: a refused sign-in fails boot with the reason", "[ledger][client]") {
    DbFixture const fixture;
    ledger::testing::IssuerScope const issuer;
    LocalClient client;
    BootController boot{client.wiring(),
                        BootOptions{.principal = "system:report-runner", .remote = true, .seed = false, .ledger = 1}};

    REQUIRE(client.settle([&boot] { return boot.state() != BootState::SigningIn; }));

    CHECK(boot.state() == BootState::Failed);
    CHECK_THAT(boot.statusText(), Catch::Matchers::ContainsSubstring("sign-in failed"));
    CHECK_FALSE(boot.ledger().has_value());
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake build/all && cmake --build build/all --target ladder_ledger_tests`
Expected: compile error, `'support/ledger_format.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/ledger/app/support/ledger_format.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ledger/core/types.hpp"
#include "ledger/dto/account_dto.hpp"
#include "ledger/dto/transaction_dto.hpp"

namespace ledger::client {

/// @brief One account in the accounts table.
struct AccountRow {
    /// @brief The account's id.
    std::int64_t id = 0;
    /// @brief Its name.
    std::string name;
    /// @brief Its kind's display name.
    std::string kind;
    /// @brief Its currency code.
    std::string currency;
    /// @brief Its balance, rendered exactly.
    std::string balanceText;
    /// @brief Compared by value.
    bool operator==(AccountRow const&) const = default;
};

/// @brief One journal entry in the entries list.
struct EntryRow {
    /// @brief The entry's journal id.
    std::int64_t id = 0;
    /// @brief That id as text.
    std::string idText;
    /// @brief Its description.
    std::string description;
    /// @brief Its date, ISO 8601, or empty.
    std::string dateText;
    /// @brief Compared by value.
    bool operator==(EntryRow const&) const = default;
};

/// @brief One currency line of a monthly statement.
struct StatementLine {
    /// @brief The currency code.
    std::string currency;
    /// @brief `"<n> transactions"`.
    std::string countText;
    /// @brief The total, rendered exactly.
    std::string amountText;
    /// @brief Compared by value.
    bool operator==(StatementLine const&) const = default;
};

/// @brief An account kind's display name. @param kind The kind. @return `"asset"`, `"expense"`, … .
[[nodiscard]] std::string kindText(AccountKind kind);
/// @brief The kind a display name names. @param text The name. @return The kind; `Asset` for anything unknown.
[[nodiscard]] AccountKind kindFromText(std::string_view text);
/// @brief Every kind's display name, in enum order. @return The names.
[[nodiscard]] std::vector<std::string> kindNames();
/// @brief A whole number no smaller than @p minimum.
/// @param text The text, digits only.
/// @param minimum The smallest accepted value.
/// @return The number, or empty.
[[nodiscard]] std::optional<std::int64_t> parseAtLeast(std::string_view text, std::int64_t minimum);
/// @brief A positive whole number (an id). @param text The text. @return The number, or empty.
[[nodiscard]] std::optional<std::int64_t> parsePositive(std::string_view text);
/// @brief Whether @p text is a `YYYY-MM` month. @param text The text. @return True for months 01–12.
[[nodiscard]] bool isMonth(std::string_view text);
/// @brief An account as a table row. @param account The account. @return The row.
[[nodiscard]] AccountRow accountRow(AccountInfo const& account);
/// @brief A journal entry as a list row. @param entry The entry. @return The row.
[[nodiscard]] EntryRow entryRow(TransactionEntryInfo const& entry);
/// @brief A finished statement's body as display lines.
/// @param resultJson The job's `result`: a JSON array of `ReportLine`.
/// @return The lines, or empty when the body does not read.
[[nodiscard]] std::optional<std::vector<StatementLine>> statementLines(std::string_view resultJson);
/// @brief This machine's offset from UTC right now, in minutes (east positive).
/// @return The offset.
[[nodiscard]] int localOffsetMinutes();

}  // namespace ledger::client
```

Create `examples/ledger/app/support/ledger_format.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "support/ledger_format.hpp"

#include <charconv>
#include <ctime>
#include <glaze/glaze.hpp>
#include <system_error>

#include "ledger/core/money.hpp"
#include "ledger/core/units.hpp"
#include "ledger/dto/report_dto.hpp"

namespace ledger::client {

std::string kindText(AccountKind kind) {
    switch (kind) {
        case AccountKind::Asset:
            return "asset";
        case AccountKind::Expense:
            return "expense";
        case AccountKind::Revenue:
            return "revenue";
        case AccountKind::Liability:
            return "liability";
        default:
            return "asset";
    }
}

AccountKind kindFromText(std::string_view text) {
    if (text == "expense") {
        return AccountKind::Expense;
    }
    if (text == "revenue") {
        return AccountKind::Revenue;
    }
    if (text == "liability") {
        return AccountKind::Liability;
    }
    return AccountKind::Asset;
}

std::vector<std::string> kindNames() {
    return {kindText(AccountKind::Asset), kindText(AccountKind::Expense), kindText(AccountKind::Revenue),
            kindText(AccountKind::Liability)};
}

std::optional<std::int64_t> parseAtLeast(std::string_view text, std::int64_t minimum) {
    std::int64_t value = 0;
    auto const [end, failure] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || failure != std::errc{} || end != text.data() + text.size() || value < minimum) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> parsePositive(std::string_view text) { return parseAtLeast(text, 1); }

bool isMonth(std::string_view text) {
    if (text.size() != 7 || text.at(4) != '-') {
        return false;
    }
    auto const year = parseAtLeast(text.substr(0, 4), 0);
    auto const month = parseAtLeast(text.substr(5, 2), 1);
    return year.has_value() && month.has_value() && *month <= 12;
}

AccountRow accountRow(AccountInfo const& account) {
    return AccountRow{.id = account.id.hasValue() ? *account.id : 0,
                      .name = account.name,
                      .kind = kindText(account.kind),
                      .currency = std::string{currencyToCode(account.currency)},
                      .balanceText = formatMoney(account.currency, account.balance)};
}

EntryRow entryRow(TransactionEntryInfo const& entry) {
    std::int64_t const journalId = entry.id.hasValue() ? *entry.id : 0;
    return EntryRow{.id = journalId,
                    .idText = std::to_string(journalId),
                    .description = entry.description,
                    .dateText = entry.date.hasValue() ? entry.date.value->toIso8601() : std::string{}};
}

std::optional<std::vector<StatementLine>> statementLines(std::string_view resultJson) {
    std::vector<ReportLine> decoded;
    if (glz::read_json(decoded, std::string{resultJson})) {
        return std::nullopt;
    }
    std::vector<StatementLine> lines;
    for (ReportLine const& line : decoded) {
        auto const total = morph::math::Rational{morph::math::Numerator{line.numerator},
                                                 morph::math::Denominator{line.denominator},
                                                 morph::math::DecimalPlaces{line.decimalPlaces}};
        lines.push_back(StatementLine{.currency = line.currency,
                                      .countText = std::to_string(line.transactionCount) + " transactions",
                                      .amountText = formatMoney(codeToCurrency(line.currency), total)});
    }
    return lines;
}

int localOffsetMinutes() {
    std::time_t const now = std::time(nullptr);
    std::tm local{};
    std::tm utc{};
#if defined(_WIN32)
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
#else
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
#endif
    // mktime reads both as local wall-clock time, so their difference is the offset; the same DST flag on both keeps
    // a daylight-saving hour from being counted twice.
    utc.tm_isdst = local.tm_isdst;
    return static_cast<int>(std::difftime(std::mktime(&local), std::mktime(&utc)) / 60.0);
}

}  // namespace ledger::client
```

Create `examples/ledger/app/controllers/boot_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>

#include "app/wiring.hpp"
#include "ledger/models/auth_model.hpp"
#include "ledger/models/ledger_model.hpp"

namespace ledger::client {

/// @brief Where startup is.
enum class BootState : std::uint8_t { SigningIn, Ready, Failed };

/// @brief Who the app runs as and which book it opens.
struct BootOptions {
    /// @brief The principal (`--user`).
    std::string principal;
    /// @brief True against a server: sign in through `Login` for a signed token. Locally the principal is installed
    ///        as is.
    bool remote = false;
    /// @brief Create a "Personal" book for the principal and open it (`--seed`).
    bool seed = false;
    /// @brief The book to open without `seed`.
    std::int64_t ledger = 1;
};

/// @brief Startup: installs the session, optionally seeds a book, and names the book every tab works on.
class BootController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    /// @param options The principal, mode, seed flag and book.
    BootController(morph::examples::Wiring wiring, BootOptions options);

    /// @brief Where startup is. Tracked.
    /// @return The state.
    [[nodiscard]] BootState state() const { return _state.get(); }
    /// @brief The book every tab works on. Tracked.
    /// @return The book once ready, else empty.
    [[nodiscard]] std::optional<LedgerId> const& ledger() const { return _ledger.get(); }
    /// @brief The boot screen's line. Tracked.
    /// @return What startup is doing, or why it failed.
    [[nodiscard]] std::string statusText() const;

private:
    void signedIn();
    void ready(LedgerId book);
    void fail(std::string message);

    morph::examples::Wiring _wiring;
    BootOptions _options;
    morph::bridge::BridgeHandler<AuthModel> _auth;
    morph::bridge::BridgeHandler<LedgerModel> _creator;
    morph::reactive::Signal<BootState> _state;
    morph::reactive::Signal<std::optional<LedgerId>> _ledger;
    morph::reactive::Signal<std::string> _failure;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace ledger::client
```

Create `examples/ledger/app/controllers/boot_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/boot_controller.hpp"

#include <exception>
#include <morph/reactive/control.hpp>
#include <morph/session/session.hpp>
#include <utility>

namespace ledger::client {

BootController::BootController(morph::examples::Wiring wiring, BootOptions options)
    : _wiring{wiring},
      _options{std::move(options)},
      _auth{wiring.bridge, &wiring.callbacks},
      _creator{wiring.bridge, &wiring.callbacks},
      _state{wiring.runtime, BootState::SigningIn},
      _ledger{wiring.runtime, std::nullopt},
      _failure{wiring.runtime, std::string{}} {
    if (_options.remote) {
        _auth.execute(Login{.username = _options.principal})
            .then(_lifetime,
                  [this](LoginResult const& result) {
                      morph::session::Context session;
                      session.principal = result.principal;
                      session.token = result.token.hasValue() ? *result.token : std::string{};
                      _wiring.bridge.setDefaultSession(std::move(session));
                      signedIn();
                  })
            .onError(_lifetime, [this](std::exception_ptr const& error) {
                fail("sign-in failed: " + morph::reactive::errorMessage(error));
            });
        return;
    }
    morph::session::Context session;
    session.principal = _options.principal;
    _wiring.bridge.setDefaultSession(std::move(session));
    signedIn();
}

std::string BootController::statusText() const {
    switch (_state.get()) {
        case BootState::SigningIn:
            return "signing in as " + _options.principal + "…";
        case BootState::Ready:
            return "book " + std::to_string(_ledger.get().has_value() ? **_ledger.get() : 0);
        case BootState::Failed:
            return _failure.get();
        default:
            return {};
    }
}

void BootController::signedIn() {
    if (!_options.seed) {
        ready(LedgerId{_options.ledger});
        return;
    }
    _creator.execute(CreateLedger{.name = "Personal"})
        .then(_lifetime, [this](CreateLedgerResult const& created) { ready(created.id); })
        .onError(_lifetime, [this](std::exception_ptr const& error) {
            fail("creating the demo book failed: " + morph::reactive::errorMessage(error));
        });
}

void BootController::ready(LedgerId book) {
    _wiring.runtime.batch([&] {
        _ledger.set(book);
        _state.set(BootState::Ready);
    });
}

void BootController::fail(std::string message) {
    _wiring.runtime.batch([&] {
        _failure.set(std::move(message));
        _state.set(BootState::Failed);
    });
}

}  // namespace ledger::client
```

Re-run the configure (`cmake build/all`) so `morph_add_rung`'s globs see the new `app/` and `tests/client/` files.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_tests
./build/all/examples/ledger/ladder_ledger_tests "[ledger][client]"
```

Expected: PASS, 9 cases.

Mutation check: in `BootController`'s constructor, drop the local-mode `setDefaultSession` call. Expected FAIL in
"local mode installs --user and opens the configured book" and in "--seed creates a book…" (the model refuses an
empty principal). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/app examples/ledger/tests/client
git commit -m "wip(ledger): app library foundations and BootController (--user, --seed)

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task L3: `AccountsController` — the accounts tab

Re-expresses `LedgerPresenter`/`LedgerQmlBridge` and `LedgerView.qml`'s conditionals: the book is a
`Query<GetLedger>` keyed on the boot's book; the month's entries a `Query<ListTransactions>` keyed on the book and
the listed month; open, store and undo are `Mutation`s invalidating what they change (so an undo refreshes a listed
month, as the bridge did by hand). A store mints its `ImportOpId` with Part 6's `newUuid()`. The text inputs and the
enabled-ness of each button live here, not in the view (`IntValidator`, `inputMask` and the `enabled:` expressions
of the QML).

**Files:**
- Create: `examples/ledger/app/controllers/accounts_controller.hpp`, `examples/ledger/app/controllers/accounts_controller.cpp`
- Test: `examples/ledger/tests/client/test_accounts_controller.cpp`

**Interfaces:**
- Consumes: `BootController::ledger()` (Task L2); `accountRow`, `entryRow`, `parsePositive`, `isMonth` (Task L2);
  `morph::examples::newUuid()` (Part 6); `ledger::OpenAccount`, `GetLedger`, `StoreTransaction`, `TransactionLeg`,
  `ListTransactions`, `UndoTransaction`, `ImportOpId`; `morph::time::Timestamp::now()`; `morph::math::Rational`.
- Produces: `ledger::client::Transfer{from, to, amountMinor, description}`, `AccountsInput{NewName, Amount,
  Description, Month}`; `AccountsController(examples::Wiring, BootController const&)`: `refresh()`, `accounts()`, `entries()`,
  `busy()`, `errorText()`, `input(AccountsInput)`, `setInput(AccountsInput, std::string)`, `newKind()`,
  `setNewKind(AccountKind)`, `from()`, `setFrom(std::int64_t)`, `to()`, `setTo(std::int64_t)`, `canOpenAccount()`,
  `canStore()`, `canList()`, `openAccount()`, `store()`, `storeTransfer(Transfer)`, `list()`,
  `undo(std::int64_t journalId)`.

- [ ] **Step 1: Write the failing test**

Create `examples/ledger/tests/client/test_accounts_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "controllers/accounts_controller.hpp"
#include "controllers/boot_controller.hpp"
#include "ledger_client_support.hpp"
#include "testkit/db_fixture.hpp"

using ledger::AccountKind;
using ledger::client::AccountRow;
using ledger::client::AccountsController;
using ledger::client::AccountsInput;
using ledger::client::BootController;
using ledger::client::BootOptions;
using ledger::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

namespace {

[[nodiscard]] BootOptions bookOptions(ledger::LedgerId book) {
    return BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = *book};
}

[[nodiscard]] std::string balanceOf(AccountsController const& accounts, std::string const& name) {
    for (AccountRow const& row : accounts.accounts()) {
        if (row.name == name) {
            return row.balanceText;
        }
    }
    return "missing";
}

}  // namespace

TEST_CASE("ledger::client::AccountsController: a fresh book is empty, then shows an opened account",
          "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    BootController boot{client.wiring(), bookOptions(book)};
    AccountsController accounts{client.wiring(), boot};
    REQUIRE(client.settle([&accounts] { return !accounts.busy(); }));
    CHECK(accounts.accounts().empty());
    CHECK_FALSE(accounts.canOpenAccount());

    accounts.setInput(AccountsInput::NewName, "Checking");
    accounts.setNewKind(AccountKind::Asset);
    REQUIRE(accounts.canOpenAccount());
    accounts.openAccount();

    CHECK(accounts.input(AccountsInput::NewName).empty());
    REQUIRE(client.settle([&accounts] { return accounts.accounts().size() == 1; }));
    AccountRow const row = accounts.accounts().front();
    CHECK(row.name == "Checking");
    CHECK(row.kind == "asset");
    CHECK(row.currency == "USD");
    CHECK(row.id > 0);
}

TEST_CASE("ledger::client::AccountsController: a transfer carries balances exactly, never as a float",
          "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    auto const checking = ledger::testing::openAccountDirect(book, "Checking", AccountKind::Asset);
    auto const groceries = ledger::testing::openAccountDirect(book, "Groceries", AccountKind::Expense);
    LocalClient client{"alice"};
    BootController boot{client.wiring(), bookOptions(book)};
    AccountsController accounts{client.wiring(), boot};
    REQUIRE(client.settle([&accounts] { return accounts.accounts().size() == 2; }));

    accounts.setFrom(*checking);
    accounts.setTo(*groceries);
    accounts.setInput(AccountsInput::Amount, "5000");
    accounts.setInput(AccountsInput::Description, "Weekly shop");
    REQUIRE(accounts.canStore());
    accounts.store();

    REQUIRE(client.settle([&accounts] { return balanceOf(accounts, "Checking") == "-50"; }));
    CHECK(balanceOf(accounts, "Groceries") == "50");
    CHECK(accounts.errorText().empty());
}

TEST_CASE("ledger::client::AccountsController: a model refusal surfaces on errorText", "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    BootController boot{client.wiring(), bookOptions(book)};
    AccountsController accounts{client.wiring(), boot};

    accounts.storeTransfer(ledger::client::Transfer{.from = ledger::AccountId{999999},
                                                    .to = ledger::AccountId{999998},
                                                    .amountMinor = 100,
                                                    .description = "nowhere"});

    REQUIRE(client.settle([&accounts] { return !accounts.errorText().empty(); }));
}

TEST_CASE("ledger::client::AccountsController: a listed month shows its entries, and Undo reverses one",
          "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    auto const checking = ledger::testing::openAccountDirect(book, "Checking", AccountKind::Asset);
    auto const groceries = ledger::testing::openAccountDirect(book, "Groceries", AccountKind::Expense);
    LocalClient client{"alice"};
    BootController boot{client.wiring(), bookOptions(book)};
    AccountsController accounts{client.wiring(), boot};
    accounts.storeTransfer(
        ledger::client::Transfer{.from = checking, .to = groceries, .amountMinor = 5000, .description = "Weekly shop"});
    REQUIRE(client.settle([&accounts] { return balanceOf(accounts, "Checking") == "-50"; }));

    accounts.setInput(AccountsInput::Month, ledger::testing::currentMonth());
    REQUIRE(accounts.canList());
    accounts.list();
    REQUIRE(client.settle([&accounts] { return accounts.entries().size() == 1; }));
    CHECK(accounts.entries().front().description == "Weekly shop");

    accounts.undo(accounts.entries().front().id);

    REQUIRE(client.settle([&accounts] { return accounts.entries().size() == 2; }));
    REQUIRE(client.settle([&accounts] { return balanceOf(accounts, "Checking") == "0"; }));
    CHECK(balanceOf(accounts, "Groceries") == "0");
}

TEST_CASE("ledger::client::AccountsController: buttons are enabled only by valid input", "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    BootController boot{client.wiring(), bookOptions(book)};
    AccountsController accounts{client.wiring(), boot};
    accounts.setFrom(1);
    accounts.setTo(2);

    CHECK_FALSE(accounts.canStore());
    accounts.setInput(AccountsInput::Amount, "0");
    CHECK_FALSE(accounts.canStore());
    accounts.setInput(AccountsInput::Amount, "abc");
    CHECK_FALSE(accounts.canStore());
    accounts.setInput(AccountsInput::Amount, "250");
    CHECK(accounts.canStore());
    accounts.setInput(AccountsInput::Month, "2026-13");
    CHECK_FALSE(accounts.canList());
    accounts.setInput(AccountsInput::Month, "2026-01");
    CHECK(accounts.canList());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_ledger_tests`
Expected: compile error, `'controllers/accounts_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/ledger/app/controllers/accounts_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <vector>

#include "app/wiring.hpp"
#include "controllers/boot_controller.hpp"
#include "ledger/models/ledger_model.hpp"
#include "support/ledger_format.hpp"

namespace ledger::client {

/// @brief A transfer between two accounts of the open book.
struct Transfer {
    /// @brief The account debited.
    AccountId from;
    /// @brief The account credited.
    AccountId to;
    /// @brief The amount in minor units (cents).
    std::int64_t amountMinor = 0;
    /// @brief The entry's description.
    std::string description;
};

/// @brief The accounts tab's text inputs.
enum class AccountsInput : std::uint8_t { NewName, Amount, Description, Month };

/// @brief The accounts tab: the book's accounts, opening one, storing a transfer, a month's entries, and undo.
class AccountsController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    /// @param boot Names the book. Borrowed: it must outlive this controller.
    AccountsController(morph::examples::Wiring wiring, BootController const& boot);

    /// @brief Fetches the book again.
    void refresh();
    /// @brief The book's accounts. Tracked. @return One row per account.
    [[nodiscard]] std::vector<AccountRow> accounts() const;
    /// @brief The listed month's entries, oldest first. Tracked. @return One row per entry.
    [[nodiscard]] std::vector<EntryRow> entries() const;
    /// @brief Whether anything is in flight. Tracked. @return True while a fetch or a write is.
    [[nodiscard]] bool busy() const;
    /// @brief The first failure among writes and fetches. Tracked. @return The message, or empty.
    [[nodiscard]] std::string errorText() const;
    /// @brief A text input's value. Tracked. @param which The input. @return Its text.
    [[nodiscard]] std::string const& input(AccountsInput which) const;
    /// @brief Sets a text input. @param which The input. @param text Its new text.
    void setInput(AccountsInput which, std::string text);
    /// @brief The new account's kind. Tracked. @return The kind.
    [[nodiscard]] AccountKind newKind() const { return _newKind.get(); }
    /// @brief Sets the new account's kind. @param kind The kind.
    void setNewKind(AccountKind kind) { _newKind.set(kind); }
    /// @brief The account a transfer debits. Tracked. @return Its id, or empty.
    [[nodiscard]] std::optional<std::int64_t> const& from() const { return _from.get(); }
    /// @brief Chooses the account a transfer debits. @param accountId The account.
    void setFrom(std::int64_t accountId) { _from.set(accountId); }
    /// @brief The account a transfer credits. Tracked. @return Its id, or empty.
    [[nodiscard]] std::optional<std::int64_t> const& to() const { return _to.get(); }
    /// @brief Chooses the account a transfer credits. @param accountId The account.
    void setTo(std::int64_t accountId) { _to.set(accountId); }
    /// @brief Whether "Open account" can run. Tracked. @return True with a book and a name.
    [[nodiscard]] bool canOpenAccount() const;
    /// @brief Whether "Store" can run. Tracked. @return True with a book, both accounts and a positive amount.
    [[nodiscard]] bool canStore() const;
    /// @brief Whether "List" can run. Tracked. @return True with a book and a `YYYY-MM` month.
    [[nodiscard]] bool canList() const;
    /// @brief Opens an account from the inputs (USD) and clears the name.
    void openAccount();
    /// @brief Stores a transfer from the inputs.
    void store();
    /// @brief Stores @p transfer as one two-legged entry dated now, under a fresh import op id.
    /// @param transfer The transfer.
    void storeTransfer(Transfer transfer);
    /// @brief Lists the input month's entries (again, when it is already listed).
    void list();
    /// @brief Reverses an entry with a compensating one.
    /// @param journalId The entry.
    void undo(std::int64_t journalId);

private:
    static constexpr std::size_t kInputs = 4;

    morph::examples::Wiring _wiring;
    BootController const* _boot;
    morph::bridge::BridgeHandler<LedgerModel, morph::bridge::AllowShared> _handler;
    std::array<std::unique_ptr<morph::reactive::Signal<std::string>>, kInputs> _inputs;
    morph::reactive::Signal<AccountKind> _newKind;
    morph::reactive::Signal<std::optional<std::int64_t>> _from;
    morph::reactive::Signal<std::optional<std::int64_t>> _to;
    morph::reactive::Signal<std::optional<std::string>> _listedMonth;
    morph::reactive::Query<GetLedger> _ledger;
    morph::reactive::Query<ListTransactions> _entries;
    morph::reactive::Mutation<OpenAccount> _open;
    morph::reactive::Mutation<StoreTransaction> _store;
    morph::reactive::Mutation<UndoTransaction> _undo;
};

}  // namespace ledger::client
```

Create `examples/ledger/app/controllers/accounts_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/accounts_controller.hpp"

#include <exception>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <utility>

#include "app/uuid.hpp"

namespace ledger::client {

namespace {

// The legs are written in cents: numerator = minor units, scale = two decimal places (the old client's encoding).
constexpr std::uint32_t kMinorUnitPlaces = 2;

using Text = morph::reactive::Signal<std::string>;

[[nodiscard]] std::array<std::unique_ptr<Text>, 4> makeInputs(morph::reactive::Runtime& runtime) {
    return {std::make_unique<Text>(runtime, std::string{}), std::make_unique<Text>(runtime, std::string{}),
            std::make_unique<Text>(runtime, std::string{}), std::make_unique<Text>(runtime, std::string{})};
}

[[nodiscard]] morph::math::Rational cents(std::int64_t minor) {
    return morph::math::Rational{morph::math::Numerator{minor}, morph::math::Denominator{1},
                                 morph::math::DecimalPlaces{kMinorUnitPlaces}};
}

}  // namespace

AccountsController::AccountsController(morph::examples::Wiring wiring, BootController const& boot)
    : _wiring{wiring},
      _boot{&boot},
      _handler{wiring.bridge, &wiring.callbacks},
      _inputs{makeInputs(wiring.runtime)},
      _newKind{wiring.runtime, AccountKind::Asset},
      _from{wiring.runtime, std::nullopt},
      _to{wiring.runtime, std::nullopt},
      _listedMonth{wiring.runtime, std::nullopt},
      _ledger{wiring.runtime, _handler,
              [this]() -> std::optional<GetLedger> {
                  auto const& book = _boot->ledger();
                  if (!book) {
                      return std::nullopt;
                  }
                  return GetLedger{.ledgerId = *book};
              }},
      _entries{wiring.runtime, _handler,
               [this]() -> std::optional<ListTransactions> {
                   auto const& book = _boot->ledger();
                   auto const& month = _listedMonth.get();
                   if (!book || !month) {
                       return std::nullopt;
                   }
                   return ListTransactions{.ledgerId = *book, .month = *month};
               }},
      _open{wiring.runtime, _handler, morph::reactive::MutationOptions{.invalidates = {&_ledger}}},
      _store{wiring.runtime, _handler, morph::reactive::MutationOptions{.invalidates = {&_ledger, &_entries}}},
      _undo{wiring.runtime, _handler, morph::reactive::MutationOptions{.invalidates = {&_ledger, &_entries}}} {}

void AccountsController::refresh() { _ledger.refetch(); }

std::vector<AccountRow> AccountsController::accounts() const {
    std::vector<AccountRow> rows;
    if (auto const& book = _ledger.value()) {
        for (AccountInfo const& account : book->accounts) {
            rows.push_back(accountRow(account));
        }
    }
    return rows;
}

std::vector<EntryRow> AccountsController::entries() const {
    std::vector<EntryRow> rows;
    if (auto const& listed = _entries.value()) {
        for (TransactionEntryInfo const& entry : listed->entries) {
            rows.push_back(entryRow(entry));
        }
    }
    return rows;
}

bool AccountsController::busy() const {
    return _ledger.pending() || _entries.pending() || _open.pending() || _store.pending() || _undo.pending();
}

std::string AccountsController::errorText() const {
    for (std::exception_ptr const& error :
         {_open.error(), _store.error(), _undo.error(), _ledger.error(), _entries.error()}) {
        if (error != nullptr) {
            return morph::reactive::errorMessage(error);
        }
    }
    return {};
}

std::string const& AccountsController::input(AccountsInput which) const {
    return _inputs.at(static_cast<std::size_t>(which))->get();
}

void AccountsController::setInput(AccountsInput which, std::string text) {
    _inputs.at(static_cast<std::size_t>(which))->set(std::move(text));
}

bool AccountsController::canOpenAccount() const {
    return _boot->ledger().has_value() && !input(AccountsInput::NewName).empty();
}

bool AccountsController::canStore() const {
    return _boot->ledger().has_value() && _from.get().has_value() && _to.get().has_value() &&
           parsePositive(input(AccountsInput::Amount)).has_value();
}

bool AccountsController::canList() const {
    return _boot->ledger().has_value() && isMonth(input(AccountsInput::Month));
}

void AccountsController::openAccount() {
    auto const& book = _boot->ledger();
    if (!book || !canOpenAccount()) {
        return;
    }
    _open.run(OpenAccount{
        .ledgerId = *book, .name = input(AccountsInput::NewName), .kind = _newKind.peek(), .currency = Currency::USD});
    setInput(AccountsInput::NewName, std::string{});
}

void AccountsController::store() {
    std::optional<std::int64_t> const amount = parsePositive(input(AccountsInput::Amount));
    std::optional<std::int64_t> const debited = _from.peek();
    std::optional<std::int64_t> const credited = _to.peek();
    if (!amount || !debited || !credited) {
        return;
    }
    storeTransfer(Transfer{.from = AccountId{*debited},
                           .to = AccountId{*credited},
                           .amountMinor = *amount,
                           .description = input(AccountsInput::Description)});
}

void AccountsController::storeTransfer(Transfer transfer) {
    auto const& book = _boot->ledger();
    if (!book) {
        return;
    }
    _store.run(StoreTransaction{
        .ledgerId = *book,
        .description = std::move(transfer.description),
        .date = morph::time::Timestamp::now(),
        .legs = {TransactionLeg{.accountId = transfer.from, .amount = cents(-transfer.amountMinor)},
                 TransactionLeg{.accountId = transfer.to, .amount = cents(transfer.amountMinor)}},
        .opId = ImportOpId{morph::examples::newUuid()}});
}

void AccountsController::list() {
    if (!canList()) {
        return;
    }
    std::string const month = input(AccountsInput::Month);
    if (_listedMonth.peek() == month) {
        _entries.refetch();
        return;
    }
    _listedMonth.set(month);
}

void AccountsController::undo(std::int64_t journalId) {
    auto const& book = _boot->ledger();
    if (!book) {
        return;
    }
    _undo.run(UndoTransaction{.ledgerId = *book, .journalId = JournalId{journalId}});
}

}  // namespace ledger::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_tests
./build/all/examples/ledger/ladder_ledger_tests "[ledger][client]"
```

Expected: PASS.

Mutation check: remove `&_entries` from `_undo`'s `invalidates`. Expected FAIL in "a listed month shows its
entries, and Undo reverses one" (the entries list never gains the reversal). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/app/controllers/accounts_controller.hpp \
        examples/ledger/app/controllers/accounts_controller.cpp \
        examples/ledger/tests/client/test_accounts_controller.cpp
git commit -m "wip(ledger): AccountsController: accounts, transfers, a month's entries, undo

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task L4: `BudgetController` — the budgets tab

Re-expresses `BudgetPresenter`/`BudgetQmlBridge` and `BudgetView.qml`: category, account link, budget and limit
are `Mutation`s on the book-keyed `BudgetModel` handler; the report is a `Query<GetBudgetReport>` keyed on the last
"Report" request; the "id N" labels and the status line ("Category created (id N)", "Budget created (id N)", "Limit
set") are projections of the mutations' last results. A limit is written in minor units at the currency's scale, as
the bridge did.

**Files:**
- Create: `examples/ledger/app/controllers/budget_controller.hpp`, `examples/ledger/app/controllers/budget_controller.cpp`
- Test: `examples/ledger/tests/client/test_budget_controller.cpp`

**Interfaces:**
- Consumes: `BootController::ledger()`, `parsePositive`, `parseAtLeast`, `isMonth` (Task L2);
  `ledger::BudgetModel`, `CreateCategory`, `LinkAccountToCategory`, `CreateBudget`, `SetBudgetLimit`,
  `GetBudgetReport`, `GetBudgetReportResult`; `formatMoney`, `currencyDecimalPlaces`, `currencyToCode`.
- Produces: `ledger::client::BudgetInput{CategoryName, LinkAccount, LinkCategory, BudgetName, BudgetCategory,
  LimitBudget, LimitMonth, LimitMinor}`; `BudgetController(examples::Wiring, BootController const&)`: `input`, `setInput`,
  `canCreateCategory()`, `canLink()`, `canCreateBudget()`, `canSetLimit()`, `canReport()`, `createCategory()`,
  `linkAccount()`, `createBudget()`, `setLimit()`, `report()`, `lastCategoryId()`, `lastBudgetId()`,
  `lastCategoryText()`, `lastBudgetText()`, `statusText()`, `limitText()`, `spentText()`, `currencyText()`, `busy()`,
  `errorText()`.

- [ ] **Step 1: Write the failing test**

Create `examples/ledger/tests/client/test_budget_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "controllers/boot_controller.hpp"
#include "controllers/budget_controller.hpp"
#include "ledger_client_support.hpp"
#include "testkit/db_fixture.hpp"

using ledger::client::BootController;
using ledger::client::BootOptions;
using ledger::client::BudgetController;
using ledger::client::BudgetInput;
using ledger::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

TEST_CASE("ledger::client::BudgetController: a category, a budget and a month's limit report exactly",
          "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = *book}};
    BudgetController budget{client.wiring(), boot};

    budget.setInput(BudgetInput::CategoryName, "Groceries");
    REQUIRE(budget.canCreateCategory());
    budget.createCategory();
    // The status line is written by an effect, which runs in the flush after the reply: wait for it, not the id.
    REQUIRE(client.settle([&budget] { return !budget.statusText().empty(); }));
    REQUIRE(budget.lastCategoryId().has_value());
    std::string const category = std::to_string(*budget.lastCategoryId());
    CHECK(budget.lastCategoryText() == "id " + category);
    CHECK(budget.statusText() == "Category created (id " + category + ")");

    budget.setInput(BudgetInput::BudgetName, "Monthly groceries");
    budget.setInput(BudgetInput::BudgetCategory, category);
    REQUIRE(budget.canCreateBudget());
    budget.createBudget();
    REQUIRE(client.settle([&budget] { return budget.lastBudgetId().has_value(); }));
    std::string const budgetId = std::to_string(*budget.lastBudgetId());

    budget.setInput(BudgetInput::LimitBudget, budgetId);
    budget.setInput(BudgetInput::LimitMonth, "2026-01");
    budget.setInput(BudgetInput::LimitMinor, "30000");
    REQUIRE(budget.canSetLimit());
    budget.setLimit();
    REQUIRE(client.settle([&budget] { return budget.statusText() == "Limit set"; }));

    REQUIRE(budget.canReport());
    budget.report();
    REQUIRE(client.settle([&budget] { return budget.limitText() != "-"; }));
    CHECK(budget.limitText() == "300");
    CHECK(budget.spentText() == "0");
    CHECK(budget.currencyText() == "USD");
    CHECK(budget.errorText().empty());
}

TEST_CASE("ledger::client::BudgetController: a refused report surfaces on errorText", "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = *book}};
    BudgetController budget{client.wiring(), boot};
    budget.setInput(BudgetInput::LimitBudget, "999999");
    budget.setInput(BudgetInput::LimitMonth, "2026-01");

    budget.report();

    REQUIRE(client.settle([&budget] { return !budget.errorText().empty(); }));
    CHECK(budget.limitText() == "-");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_ledger_tests`
Expected: compile error, `'controllers/budget_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/ledger/app/controllers/budget_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>

#include "app/wiring.hpp"
#include "controllers/boot_controller.hpp"
#include "ledger/models/budget_model.hpp"

namespace ledger::client {

/// @brief The budgets tab's text inputs.
enum class BudgetInput : std::uint8_t {
    CategoryName,
    LinkAccount,
    LinkCategory,
    BudgetName,
    BudgetCategory,
    LimitBudget,
    LimitMonth,
    LimitMinor
};

/// @brief The budgets tab: categories, account links, budgets, monthly limits and the month's report.
class BudgetController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    /// @param boot Names the book. Borrowed: it must outlive this controller.
    BudgetController(morph::examples::Wiring wiring, BootController const& boot);
    ~BudgetController();
    BudgetController(BudgetController const&) = delete;
    BudgetController& operator=(BudgetController const&) = delete;
    BudgetController(BudgetController&&) = delete;
    BudgetController& operator=(BudgetController&&) = delete;

    /// @brief A text input's value. Tracked. @param which The input. @return Its text.
    [[nodiscard]] std::string const& input(BudgetInput which) const;
    /// @brief Sets a text input. @param which The input. @param text Its new text.
    void setInput(BudgetInput which, std::string text);
    /// @brief Tracked. @return Whether "Create category" can run (a book and a name).
    [[nodiscard]] bool canCreateCategory() const;
    /// @brief Tracked. @return Whether "Link" can run (an account id and a category id).
    [[nodiscard]] bool canLink() const;
    /// @brief Tracked. @return Whether "Create budget" can run (a book, a name and a category id).
    [[nodiscard]] bool canCreateBudget() const;
    /// @brief Tracked. @return Whether "Set limit" can run (a budget id, a month and a non-negative amount).
    [[nodiscard]] bool canSetLimit() const;
    /// @brief Tracked. @return Whether "Report" can run (a budget id and a month).
    [[nodiscard]] bool canReport() const;
    /// @brief Creates a category in the book from the inputs.
    void createCategory();
    /// @brief Links an account to a category from the inputs.
    void linkAccount();
    /// @brief Creates a budget for a category from the inputs.
    void createBudget();
    /// @brief Sets a month's limit (USD, minor units) from the inputs.
    void setLimit();
    /// @brief Fetches the month's report for the limit inputs' budget.
    void report();
    /// @brief Tracked. @return The last created category's id, or empty.
    [[nodiscard]] std::optional<std::int64_t> lastCategoryId() const;
    /// @brief Tracked. @return The last created budget's id, or empty.
    [[nodiscard]] std::optional<std::int64_t> lastBudgetId() const;
    /// @brief Tracked. @return `"id <n>"` for the last category, or empty.
    [[nodiscard]] std::string lastCategoryText() const;
    /// @brief Tracked. @return `"id <n>"` for the last budget, or empty.
    [[nodiscard]] std::string lastBudgetText() const;
    /// @brief Tracked. @return What the last write did, or empty.
    [[nodiscard]] std::string const& statusText() const { return _status.get(); }
    /// @brief Tracked. @return The report's limit, exactly, or `"-"`.
    [[nodiscard]] std::string limitText() const;
    /// @brief Tracked. @return The report's spend, exactly, or `"-"`.
    [[nodiscard]] std::string spentText() const;
    /// @brief Tracked. @return The report's currency code, or `"-"`.
    [[nodiscard]] std::string currencyText() const;
    /// @brief Tracked. @return Whether a write or the report is in flight.
    [[nodiscard]] bool busy() const;
    /// @brief Tracked. @return The first failure, or empty.
    [[nodiscard]] std::string errorText() const;

private:
    static constexpr std::size_t kInputs = 8;

    morph::examples::Wiring _wiring;
    BootController const* _boot;
    morph::bridge::BridgeHandler<BudgetModel, morph::bridge::AllowShared> _handler;
    std::array<std::unique_ptr<morph::reactive::Signal<std::string>>, kInputs> _inputs;
    morph::reactive::Signal<std::string> _status;
    morph::reactive::Signal<std::optional<GetBudgetReport>> _reportRequest;
    morph::reactive::Mutation<CreateCategory> _createCategory;
    morph::reactive::Mutation<LinkAccountToCategory> _link;
    morph::reactive::Mutation<CreateBudget> _createBudget;
    morph::reactive::Mutation<SetBudgetLimit> _setLimit;
    morph::reactive::Query<GetBudgetReport> _report;
    std::unique_ptr<morph::reactive::Effect> _onCategory;
    std::unique_ptr<morph::reactive::Effect> _onBudget;
    std::unique_ptr<morph::reactive::Effect> _onLimit;
};

}  // namespace ledger::client
```

Create `examples/ledger/app/controllers/budget_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/budget_controller.hpp"

#include <exception>
#include <morph/util/rational.hpp>
#include <utility>

#include "ledger/core/money.hpp"
#include "ledger/core/units.hpp"
#include "support/ledger_format.hpp"

namespace ledger::client {

namespace {

using Text = morph::reactive::Signal<std::string>;

[[nodiscard]] std::array<std::unique_ptr<Text>, 8> makeInputs(morph::reactive::Runtime& runtime) {
    std::array<std::unique_ptr<Text>, 8> inputs;
    for (auto& input : inputs) {
        input = std::make_unique<Text>(runtime, std::string{});
    }
    return inputs;
}

template <typename Id>
[[nodiscard]] std::optional<std::int64_t> idOf(std::optional<Id> const& created) {
    if (!created || !created->hasValue()) {
        return std::nullopt;
    }
    return **created;
}

}  // namespace

BudgetController::BudgetController(morph::examples::Wiring wiring, BootController const& boot)
    : _wiring{wiring},
      _boot{&boot},
      _handler{wiring.bridge, &wiring.callbacks},
      _inputs{makeInputs(wiring.runtime)},
      _status{wiring.runtime, std::string{}},
      _reportRequest{wiring.runtime, std::nullopt},
      _createCategory{wiring.runtime, _handler},
      _link{wiring.runtime, _handler},
      _createBudget{wiring.runtime, _handler},
      _setLimit{wiring.runtime, _handler},
      _report{wiring.runtime, _handler, [this] { return _reportRequest.get(); }} {
    auto const announce = [this](std::string text) {
        _wiring.runtime.untracked([&] { _status.set(std::move(text)); });
    };
    _onCategory = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this, announce] {
        if (std::optional<std::int64_t> const created = idOf(_createCategory.lastResult())) {
            announce("Category created (id " + std::to_string(*created) + ")");
        }
    });
    _onBudget = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this, announce] {
        if (std::optional<std::int64_t> const created = idOf(_createBudget.lastResult())) {
            announce("Budget created (id " + std::to_string(*created) + ")");
        }
    });
    _onLimit = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this, announce] {
        if (_setLimit.lastResult().has_value()) {
            announce("Limit set");
        }
    });
}

BudgetController::~BudgetController() = default;

std::string const& BudgetController::input(BudgetInput which) const {
    return _inputs.at(static_cast<std::size_t>(which))->get();
}

void BudgetController::setInput(BudgetInput which, std::string text) {
    _inputs.at(static_cast<std::size_t>(which))->set(std::move(text));
}

bool BudgetController::canCreateCategory() const {
    return _boot->ledger().has_value() && !input(BudgetInput::CategoryName).empty();
}

bool BudgetController::canLink() const {
    return parsePositive(input(BudgetInput::LinkAccount)).has_value() &&
           parsePositive(input(BudgetInput::LinkCategory)).has_value();
}

bool BudgetController::canCreateBudget() const {
    return _boot->ledger().has_value() && !input(BudgetInput::BudgetName).empty() &&
           parsePositive(input(BudgetInput::BudgetCategory)).has_value();
}

bool BudgetController::canSetLimit() const {
    return canReport() && parseAtLeast(input(BudgetInput::LimitMinor), 0).has_value();
}

bool BudgetController::canReport() const {
    return parsePositive(input(BudgetInput::LimitBudget)).has_value() && isMonth(input(BudgetInput::LimitMonth));
}

void BudgetController::createCategory() {
    auto const& book = _boot->ledger();
    if (!book || !canCreateCategory()) {
        return;
    }
    _createCategory.run(CreateCategory{.ledgerId = *book, .name = input(BudgetInput::CategoryName)});
    setInput(BudgetInput::CategoryName, std::string{});
}

void BudgetController::linkAccount() {
    auto const account = parsePositive(input(BudgetInput::LinkAccount));
    auto const category = parsePositive(input(BudgetInput::LinkCategory));
    if (!account || !category) {
        return;
    }
    _link.run(LinkAccountToCategory{.accountId = AccountId{*account}, .categoryId = CategoryId{*category}});
}

void BudgetController::createBudget() {
    auto const& book = _boot->ledger();
    auto const category = parsePositive(input(BudgetInput::BudgetCategory));
    if (!book || !category || input(BudgetInput::BudgetName).empty()) {
        return;
    }
    _createBudget.run(
        CreateBudget{.ledgerId = *book, .name = input(BudgetInput::BudgetName), .categoryId = CategoryId{*category}});
}

void BudgetController::setLimit() {
    auto const target = parsePositive(input(BudgetInput::LimitBudget));
    auto const minor = parseAtLeast(input(BudgetInput::LimitMinor), 0);
    if (!target || !minor || !isMonth(input(BudgetInput::LimitMonth))) {
        return;
    }
    auto const limit = morph::math::Rational{morph::math::Numerator{*minor}, morph::math::Denominator{1},
                                             morph::math::DecimalPlaces{currencyDecimalPlaces(Currency::USD)}};
    _setLimit.run(SetBudgetLimit{
        .budgetId = BudgetId{*target}, .month = input(BudgetInput::LimitMonth), .limit = limit, .currency = Currency::USD});
}

void BudgetController::report() {
    auto const target = parsePositive(input(BudgetInput::LimitBudget));
    if (!target) {
        return;
    }
    _reportRequest.set(GetBudgetReport{.budgetId = BudgetId{*target}, .month = input(BudgetInput::LimitMonth)});
}

std::optional<std::int64_t> BudgetController::lastCategoryId() const { return idOf(_createCategory.lastResult()); }

std::optional<std::int64_t> BudgetController::lastBudgetId() const { return idOf(_createBudget.lastResult()); }

std::string BudgetController::lastCategoryText() const {
    auto const created = lastCategoryId();
    return created ? "id " + std::to_string(*created) : std::string{};
}

std::string BudgetController::lastBudgetText() const {
    auto const created = lastBudgetId();
    return created ? "id " + std::to_string(*created) : std::string{};
}

std::string BudgetController::limitText() const {
    auto const& result = _report.value();
    return result ? formatMoney(result->currency, result->limit) : std::string{"-"};
}

std::string BudgetController::spentText() const {
    auto const& result = _report.value();
    return result ? formatMoney(result->currency, result->spent) : std::string{"-"};
}

std::string BudgetController::currencyText() const {
    auto const& result = _report.value();
    return result ? std::string{currencyToCode(result->currency)} : std::string{"-"};
}

bool BudgetController::busy() const {
    return _createCategory.pending() || _link.pending() || _createBudget.pending() || _setLimit.pending() ||
           _report.pending();
}

std::string BudgetController::errorText() const {
    for (std::exception_ptr const& error :
         {_createCategory.error(), _link.error(), _createBudget.error(), _setLimit.error(), _report.error()}) {
        if (error != nullptr) {
            return morph::reactive::errorMessage(error);
        }
    }
    return {};
}

}  // namespace ledger::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_tests
./build/all/examples/ledger/ladder_ledger_tests "[ledger][client]"
```

Expected: PASS.

Mutation check: in `setLimit`, write the limit at `DecimalPlaces{0}`. Expected FAIL in "a category, a budget and a
month's limit report exactly" (`limitText()` is `"30000"`). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/app/controllers/budget_controller.hpp examples/ledger/app/controllers/budget_controller.cpp \
        examples/ledger/tests/client/test_budget_controller.cpp
git commit -m "wip(ledger): BudgetController: categories, budgets, limits and the month's report

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task L5: `RulesController` — the rules tab

Re-expresses `RulePresenter`/`RuleQmlBridge` and `RulesView.qml`: create and update are `Mutation`s on the
book-keyed `RuleModel` handler; "Last rule" shows the id after a create and the id, match and bumped version after
an update.

**Files:**
- Create: `examples/ledger/app/controllers/rules_controller.hpp`, `examples/ledger/app/controllers/rules_controller.cpp`
- Test: `examples/ledger/tests/client/test_ledger_rules_controller.cpp`

**Interfaces:**
- Consumes: `BootController::ledger()`, `parsePositive` (Task L2); `ledger::RuleModel`, `CreateRule`, `UpdateRule`,
  `RuleInfo`, `RuleTrigger`, `RuleAction`.
- Produces: `ledger::client::RuleInput{MatchText, CategoryId, EditRuleId, EditMatch, EditCategory}`;
  `RulesController(examples::Wiring, BootController const&)`: `input`, `setInput`, `canCreate()`, `canUpdate()`, `create()`,
  `update()`, `lastRuleId()`, `lastRuleMatch()`, `lastRuleVersion()`, `statusText()`, `busy()`, `errorText()`.

- [ ] **Step 1: Write the failing test**

Create `examples/ledger/tests/client/test_ledger_rules_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "controllers/boot_controller.hpp"
#include "controllers/rules_controller.hpp"
#include "ledger_client_support.hpp"
#include "testkit/db_fixture.hpp"

using ledger::client::BootController;
using ledger::client::BootOptions;
using ledger::client::RuleInput;
using ledger::client::RulesController;
using ledger::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

TEST_CASE("ledger::client::RulesController: create, then update with the bumped version shown", "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = *book}};
    RulesController rules{client.wiring(), boot};
    CHECK(rules.lastRuleId() == "-");
    CHECK_FALSE(rules.canCreate());

    rules.setInput(RuleInput::MatchText, "COFFEE");
    rules.setInput(RuleInput::CategoryId, "7");
    REQUIRE(rules.canCreate());
    rules.create();
    REQUIRE(client.settle([&rules] { return rules.lastRuleId() != "-"; }));
    CHECK(rules.statusText() == "Rule created");
    CHECK(rules.lastRuleVersion() == "-");

    rules.setInput(RuleInput::EditRuleId, rules.lastRuleId());
    rules.setInput(RuleInput::EditMatch, "ESPRESSO");
    rules.setInput(RuleInput::EditCategory, "9");
    REQUIRE(rules.canUpdate());
    rules.update();
    REQUIRE(client.settle([&rules] { return rules.statusText() == "Rule updated"; }));
    CHECK(rules.lastRuleMatch() == "ESPRESSO");
    CHECK(std::stoi(rules.lastRuleVersion()) > 1);
}

TEST_CASE("ledger::client::RulesController: a refusal surfaces on errorText", "[ledger][client]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = *book}};
    RulesController rules{client.wiring(), boot};
    rules.setInput(RuleInput::EditRuleId, "999999");
    rules.setInput(RuleInput::EditMatch, "NOTHING");

    rules.update();

    REQUIRE(client.settle([&rules] { return !rules.errorText().empty(); }));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_ledger_tests`
Expected: compile error, `'controllers/rules_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/ledger/app/controllers/rules_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>

#include "app/wiring.hpp"
#include "controllers/boot_controller.hpp"
#include "ledger/models/rule_model.hpp"

namespace ledger::client {

/// @brief The rules tab's text inputs.
enum class RuleInput : std::uint8_t { MatchText, CategoryId, EditRuleId, EditMatch, EditCategory };

/// @brief The rules tab: create a categorisation rule, edit one (which bumps its version), and show the last one.
class RulesController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    /// @param boot Names the book. Borrowed: it must outlive this controller.
    RulesController(morph::examples::Wiring wiring, BootController const& boot);
    ~RulesController();
    RulesController(RulesController const&) = delete;
    RulesController& operator=(RulesController const&) = delete;
    RulesController(RulesController&&) = delete;
    RulesController& operator=(RulesController&&) = delete;

    /// @brief A text input's value. Tracked. @param which The input. @return Its text.
    [[nodiscard]] std::string const& input(RuleInput which) const;
    /// @brief Sets a text input. @param which The input. @param text Its new text.
    void setInput(RuleInput which, std::string text);
    /// @brief Tracked. @return Whether "Create rule" can run (a book and a match text).
    [[nodiscard]] bool canCreate() const;
    /// @brief Tracked. @return Whether "Update rule" can run (a rule id).
    [[nodiscard]] bool canUpdate() const;
    /// @brief Creates a "description contains → set category" rule from the inputs.
    void create();
    /// @brief Edits the rule the inputs name.
    void update();
    /// @brief Tracked. @return The last rule's id, or `"-"`.
    [[nodiscard]] std::string lastRuleId() const;
    /// @brief Tracked. @return The last rule's match text after an update, or `"-"`.
    [[nodiscard]] std::string lastRuleMatch() const;
    /// @brief Tracked. @return The last rule's version after an update, or `"-"`.
    [[nodiscard]] std::string lastRuleVersion() const;
    /// @brief Tracked. @return `"Rule created"`, `"Rule updated"`, or empty.
    [[nodiscard]] std::string const& statusText() const { return _status.get(); }
    /// @brief Tracked. @return Whether a write is in flight.
    [[nodiscard]] bool busy() const { return _create.pending() || _update.pending(); }
    /// @brief Tracked. @return The first failure, or empty.
    [[nodiscard]] std::string errorText() const;

private:
    struct LastRule {
        std::string id;
        std::string match;
        std::string version;
        bool operator==(LastRule const&) const = default;
    };
    static constexpr std::size_t kInputs = 5;

    morph::examples::Wiring _wiring;
    BootController const* _boot;
    morph::bridge::BridgeHandler<RuleModel, morph::bridge::AllowShared> _handler;
    std::array<std::unique_ptr<morph::reactive::Signal<std::string>>, kInputs> _inputs;
    morph::reactive::Signal<std::optional<LastRule>> _lastRule;
    morph::reactive::Signal<std::string> _status;
    morph::reactive::Mutation<CreateRule> _create;
    morph::reactive::Mutation<UpdateRule> _update;
    std::unique_ptr<morph::reactive::Effect> _onCreated;
    std::unique_ptr<morph::reactive::Effect> _onUpdated;
};

}  // namespace ledger::client
```

Create `examples/ledger/app/controllers/rules_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/rules_controller.hpp"

#include <exception>
#include <utility>

#include "support/ledger_format.hpp"

namespace ledger::client {

namespace {

using Text = morph::reactive::Signal<std::string>;

[[nodiscard]] std::array<std::unique_ptr<Text>, 5> makeInputs(morph::reactive::Runtime& runtime) {
    std::array<std::unique_ptr<Text>, 5> inputs;
    for (auto& input : inputs) {
        input = std::make_unique<Text>(runtime, std::string{});
    }
    return inputs;
}

}  // namespace

RulesController::RulesController(morph::examples::Wiring wiring, BootController const& boot)
    : _wiring{wiring},
      _boot{&boot},
      _handler{wiring.bridge, &wiring.callbacks},
      _inputs{makeInputs(wiring.runtime)},
      _lastRule{wiring.runtime, std::nullopt},
      _status{wiring.runtime, std::string{}},
      _create{wiring.runtime, _handler},
      _update{wiring.runtime, _handler} {
    _onCreated = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this] {
        auto const& created = _create.lastResult();
        if (created && created->hasValue()) {
            LastRule rule{.id = std::to_string(**created), .match = "-", .version = "-"};
            _wiring.runtime.untracked([&] {
                _wiring.runtime.batch([&] {
                    _lastRule.set(std::move(rule));
                    _status.set("Rule created");
                });
            });
        }
    });
    _onUpdated = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this] {
        auto const& updated = _update.lastResult();
        if (updated && updated->id.hasValue()) {
            LastRule rule{.id = std::to_string(*updated->id),
                          .match = updated->matchText,
                          .version = std::to_string(updated->version)};
            _wiring.runtime.untracked([&] {
                _wiring.runtime.batch([&] {
                    _lastRule.set(std::move(rule));
                    _status.set("Rule updated");
                });
            });
        }
    });
}

RulesController::~RulesController() = default;

std::string const& RulesController::input(RuleInput which) const {
    return _inputs.at(static_cast<std::size_t>(which))->get();
}

void RulesController::setInput(RuleInput which, std::string text) {
    _inputs.at(static_cast<std::size_t>(which))->set(std::move(text));
}

bool RulesController::canCreate() const {
    return _boot->ledger().has_value() && !input(RuleInput::MatchText).empty();
}

bool RulesController::canUpdate() const { return parsePositive(input(RuleInput::EditRuleId)).has_value(); }

void RulesController::create() {
    auto const& book = _boot->ledger();
    if (!book || !canCreate()) {
        return;
    }
    _create.run(CreateRule{.ledgerId = *book,
                           .trigger = RuleTrigger::DescriptionContains,
                           .matchText = input(RuleInput::MatchText),
                           .action = RuleAction::SetCategory,
                           .actionValue = input(RuleInput::CategoryId)});
}

void RulesController::update() {
    auto const rule = parsePositive(input(RuleInput::EditRuleId));
    if (!rule) {
        return;
    }
    _update.run(UpdateRule{.ruleId = RuleId{*rule},
                           .matchText = input(RuleInput::EditMatch),
                           .actionValue = input(RuleInput::EditCategory),
                           .expectedVersion = std::nullopt});
}

std::string RulesController::lastRuleId() const {
    auto const& rule = _lastRule.get();
    return rule ? rule->id : std::string{"-"};
}

std::string RulesController::lastRuleMatch() const {
    auto const& rule = _lastRule.get();
    return rule ? rule->match : std::string{"-"};
}

std::string RulesController::lastRuleVersion() const {
    auto const& rule = _lastRule.get();
    return rule ? rule->version : std::string{"-"};
}

std::string RulesController::errorText() const {
    for (std::exception_ptr const& error : {_create.error(), _update.error()}) {
        if (error != nullptr) {
            return morph::reactive::errorMessage(error);
        }
    }
    return {};
}

}  // namespace ledger::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_tests
./build/all/examples/ledger/ladder_ledger_tests "[ledger][client]"
```

Expected: PASS.

Mutation check: in `_onUpdated`, set `.version = "-"`. Expected FAIL in "create, then update with the bumped version
shown" (`std::stoi("-")` throws). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/app/controllers/rules_controller.hpp examples/ledger/app/controllers/rules_controller.cpp \
        examples/ledger/tests/client/test_ledger_rules_controller.cpp
git commit -m "wip(ledger): RulesController: create and update categorisation rules

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task L6: `StatementController` — the statement job, polled by a `Query` with `refreshEvery`

Re-expresses `ReportPresenter`, `ReportJobPoller` and `ReportQmlBridge` (spec 4 §5: "the statement job poll is a
`Query` with `refreshEvery`"). A request is a `Mutation<SubmitReport>`; its job id becomes the key of a
`Query<GetReportStatus>` that refetches every two seconds on the injected `Scheduler`; an effect settles the
outcome when the job leaves `Pending` — or the poll fails — and clears the key, which makes the query idle and so
stops the polling while the outcome (and its lines) stays. The offset the statement is computed in is a
`StatementOptions` field the application fills with `localOffsetMinutes()` (Task L9), as `ReportView.qml` took it
from the JavaScript `Date`.

**Files:**
- Create: `examples/ledger/app/controllers/statement_controller.hpp`,
  `examples/ledger/app/controllers/statement_controller.cpp`
- Test: `examples/ledger/tests/client/test_statement_controller.cpp`

**Interfaces:**
- Consumes: `BootController::ledger()`, `statementLines`, `parseAtLeast`, `StatementLine` (Task L2);
  `reactive::Query(Runtime&, Fetch, Key, QueryOptions{scheduler, refreshEvery})` (Part 1, Task 9);
  `ledger::SubmitReport`, `GetReportStatus`, `GetReportStatusResult`, `MonthlyStatementParams`, `ReportStatus`;
  `ledger::app::App` and `runPendingReportsOnce()`, `reportsInFlight()`, `stopBackgroundJobs()` (Task L1, in tests).
- Produces: `ledger::client::StatementStatus{Idle, Pending, Done, Failed}`, `StatementOptions{timezoneOffsetMinutes,
  pollEvery}`; `StatementController(examples::Wiring, BootController const&, StatementOptions)`: `year()`, `setYear`, `month()`,
  `setMonth(int)`, `canRequest()`, `request()`, `status()`, `statusText()`, `pending()`, `lines()`, `offsetText()`,
  `errorText()`, `pollCount()`.

- [ ] **Step 1: Write the failing test**

Create `examples/ledger/tests/client/test_statement_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The statement end to end: the client submits, the server-side report runner (here driven by hand) runs the job,
// and the client's poll — a Query refreshed on a manual clock — picks the result up and then stops.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/session/session.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "controllers/boot_controller.hpp"
#include "controllers/statement_controller.hpp"
#include "ledger/app/app.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger_client_support.hpp"
#include "testkit/db_fixture.hpp"

using ledger::client::BootController;
using ledger::client::BootOptions;
using ledger::client::StatementController;
using ledger::client::StatementOptions;
using ledger::client::StatementStatus;
using morph::ladder::testkit::DbFixture;
using namespace std::chrono_literals;

namespace {

// One transfer late on the 31st of January in UTC-5 — the 1st of February in UTC.
void seedLateJanuaryTransfer(ledger::LedgerId book) {
    auto const checking = ledger::testing::openAccountDirect(book, "Checking", ledger::AccountKind::Asset);
    auto const groceries = ledger::testing::openAccountDirect(book, "Groceries", ledger::AccountKind::Expense);
    morph::session::Context context;
    context.principal = "alice";
    morph::session::detail::ScopedContext const scope{context};
    auto const instant = morph::time::DateTime::fromIso8601("2026-02-01T04:30:00Z");
    REQUIRE(instant.has_value());
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    ledger::LedgerModel model;
    model.execute(ledger::StoreTransaction{
        .ledgerId = book,
        .description = "late on the 31st, local",
        .date = morph::time::Timestamp{*instant},
        .legs = {ledger::TransactionLeg{.accountId = checking,
                                        .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                        DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = groceries,
                                        .amount = morph::math::Rational{Numerator{5000}, Denominator{1},
                                                                        DecimalPlaces{2}}}}});
}

void runJobs(ledger::testing::QtClient& client, ledger::app::App& runner) {
    runner.runPendingReportsOnce();
    REQUIRE(client.settle([&runner] { return !runner.reportsInFlight(); }));
}

}  // namespace

TEST_CASE("ledger::client::StatementController: a statement is polled to Done; a Done job stops the poll and keeps "
          "its lines",
          "[ledger][client][report]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    seedLateJanuaryTransfer(book);
    ledger::app::App runner{std::string{ledger::testing::kSecret}, std::chrono::hours{1}};
    ledger::testing::QtClient client{"alice"};
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = *book}};
    StatementController statement{client.wiring(), boot,
                                  StatementOptions{.timezoneOffsetMinutes = -300, .pollEvery = 2000ms}};
    CHECK(statement.status() == StatementStatus::Idle);
    CHECK(statement.offsetText() == "Offset applied: -300 minutes from UTC");

    statement.setYear("2026");
    statement.setMonth(1);
    REQUIRE(statement.canRequest());
    statement.request();
    CHECK(statement.status() == StatementStatus::Pending);
    REQUIRE(client.settle([&statement] { return statement.pollCount() >= 1; }));
    runJobs(client, runner);
    client.scheduler.advance(2000ms);

    REQUIRE(client.settle([&statement] { return statement.status() == StatementStatus::Done; }));
    REQUIRE(statement.lines().size() == 1);
    CHECK(statement.lines().front().currency == "USD");
    CHECK(statement.lines().front().countText == "1 transactions");
    CHECK(statement.lines().front().amountText == "0");
    CHECK(statement.errorText().empty());

    std::size_t const polls = statement.pollCount();
    client.scheduler.advance(10000ms);
    CHECK(statement.pollCount() == polls);
    CHECK(statement.status() == StatementStatus::Done);
    CHECK(statement.lines().size() == 1);

    statement.setMonth(3);
    statement.request();
    REQUIRE(client.settle([&statement] { return statement.pollCount() > polls; }));
    runJobs(client, runner);
    client.scheduler.advance(2000ms);
    REQUIRE(client.settle([&statement] { return statement.status() == StatementStatus::Done; }));
    REQUIRE_FALSE(statement.lines().empty());
    CHECK(statement.lines().front().countText == "0 transactions");

    runner.stopBackgroundJobs();
    REQUIRE(client.settle([&runner] { return !runner.reportsInFlight(); }));
}

TEST_CASE("ledger::client::StatementController: a refused submission fails at once and polls nothing",
          "[ledger][client][report]") {
    DbFixture const fixture;
    ledger::testing::LocalClient client{"alice"};
    BootController boot{client.wiring(), BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = 999999}};
    StatementController statement{client.wiring(), boot, StatementOptions{.timezoneOffsetMinutes = 0, .pollEvery = 2000ms}};
    statement.setYear("2026");
    statement.setMonth(1);

    statement.request();

    REQUIRE(client.settle([&statement] { return statement.status() == StatementStatus::Failed; }));
    CHECK_FALSE(statement.errorText().empty());
    CHECK(statement.pollCount() == 0);
    client.scheduler.advance(10000ms);
    CHECK(statement.pollCount() == 0);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_ledger_tests`
Expected: compile error, `'controllers/statement_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/ledger/app/controllers/statement_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <vector>

#include "app/wiring.hpp"
#include "controllers/boot_controller.hpp"
#include "ledger/models/ledger_model.hpp"
#include "support/ledger_format.hpp"

namespace ledger::client {

/// @brief Where the statement is.
enum class StatementStatus : std::uint8_t { Idle, Pending, Done, Failed };

/// @brief How statements are computed and polled.
struct StatementOptions {
    /// @brief The display zone's offset from UTC; month boundaries are drawn in it.
    int timezoneOffsetMinutes = 0;
    /// @brief How often a pending job is polled.
    std::chrono::milliseconds pollEvery{2000};
};

/// @brief The statement tab: request a monthly statement, poll its job until it settles, show its lines.
class StatementController {
public:
    /// @param wiring The runtime, scheduler, bridge and owner this controller uses.
    /// @param boot Names the book. Borrowed: it must outlive this controller.
    /// @param options The display zone and the poll period.
    StatementController(morph::examples::Wiring wiring, BootController const& boot, StatementOptions options);
    ~StatementController();
    StatementController(StatementController const&) = delete;
    StatementController& operator=(StatementController const&) = delete;
    StatementController(StatementController&&) = delete;
    StatementController& operator=(StatementController&&) = delete;

    /// @brief Tracked. @return The year input.
    [[nodiscard]] std::string const& year() const { return _year.get(); }
    /// @brief Sets the year input. @param text The year.
    void setYear(std::string text) { _year.set(std::move(text)); }
    /// @brief Tracked. @return The month, 1–12.
    [[nodiscard]] int month() const { return _month.get(); }
    /// @brief Sets the month. @param value 1–12.
    void setMonth(int value) { _month.set(value); }
    /// @brief Tracked. @return Whether "Request" can run: a book, a year 1970–2999, a month 1–12, nothing pending.
    [[nodiscard]] bool canRequest() const;
    /// @brief Submits the monthly statement for the inputs.
    void request();
    /// @brief Tracked. @return Where the statement is.
    [[nodiscard]] StatementStatus status() const;
    /// @brief Tracked. @return `"idle"`, `"pending"`, `"done"` or `"failed"`.
    [[nodiscard]] std::string statusText() const;
    /// @brief Tracked. @return True while submitting or polling.
    [[nodiscard]] bool pending() const { return status() == StatementStatus::Pending; }
    /// @brief Tracked. @return The finished statement's lines; empty otherwise.
    [[nodiscard]] std::vector<StatementLine> lines() const;
    /// @brief The offset line. @return `"Offset applied: <n> minutes from UTC"`.
    [[nodiscard]] std::string offsetText() const;
    /// @brief Tracked. @return Why the last statement failed, or empty.
    [[nodiscard]] std::string errorText() const;
    /// @brief How many status polls were issued; for tests.
    /// @return The count.
    [[nodiscard]] std::size_t pollCount() const noexcept { return _polls; }

private:
    struct Outcome {
        bool failed = false;
        std::string message;
        std::vector<StatementLine> lines;
        bool operator==(Outcome const&) const = default;
    };

    void settle();
    void finish(Outcome outcome);

    morph::examples::Wiring _wiring;
    BootController const* _boot;
    StatementOptions _options;
    morph::bridge::BridgeHandler<LedgerModel, morph::bridge::AllowShared> _handler;
    morph::reactive::Signal<std::string> _year;
    morph::reactive::Signal<int> _month;
    morph::reactive::Signal<std::optional<ReportJobId>> _job;
    morph::reactive::Signal<std::optional<Outcome>> _outcome;
    std::size_t _polls = 0;
    morph::reactive::Mutation<SubmitReport> _submit;
    morph::reactive::Query<GetReportStatus> _status;
    std::unique_ptr<morph::reactive::Effect> _track;
    std::unique_ptr<morph::reactive::Effect> _settle;
};

}  // namespace ledger::client
```

Create `examples/ledger/app/controllers/statement_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/statement_controller.hpp"

#include <exception>
#include <glaze/glaze.hpp>
#include <utility>

#include "ledger/dto/report_dto.hpp"

namespace ledger::client {

namespace {

constexpr std::int64_t kFirstYear = 1970;
constexpr std::int64_t kLastYear = 2999;

}  // namespace

StatementController::StatementController(morph::examples::Wiring wiring, BootController const& boot,
                                         StatementOptions options)
    : _wiring{wiring},
      _boot{&boot},
      _options{options},
      _handler{wiring.bridge, &wiring.callbacks},
      _year{wiring.runtime, std::string{"2026"}},
      _month{wiring.runtime, 1},
      _job{wiring.runtime, std::nullopt},
      _outcome{wiring.runtime, std::nullopt},
      _submit{wiring.runtime, _handler},
      _status{wiring.runtime,
              [this](GetReportStatus const& action) {
                  ++_polls;
                  return _handler.execute(action);
              },
              [this]() -> std::optional<GetReportStatus> {
                  auto const& job = _job.get();
                  if (!job) {
                      return std::nullopt;
                  }
                  return GetReportStatus{.jobId = *job};
              },
              morph::reactive::QueryOptions{.scheduler = &wiring.scheduler, .refreshEvery = options.pollEvery}} {
    _track = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this] {
        auto const& submitted = _submit.lastResult();
        if (submitted) {
            ReportJobId const job = *submitted;
            _wiring.runtime.untracked([&] { _job.set(job); });
        }
    });
    _settle = std::make_unique<morph::reactive::Effect>(_wiring.runtime, [this] { settle(); });
}

StatementController::~StatementController() = default;

bool StatementController::canRequest() const {
    auto const yearValue = parseAtLeast(_year.get(), kFirstYear);
    int const monthValue = _month.get();
    return _boot->ledger().has_value() && yearValue.has_value() && *yearValue <= kLastYear && monthValue >= 1 &&
           monthValue <= 12 && !pending();
}

void StatementController::request() {
    auto const& book = _boot->ledger();
    auto const yearValue = parseAtLeast(_year.peek(), kFirstYear);
    if (!book || !yearValue || pending()) {
        return;
    }
    MonthlyStatementParams const params{.year = static_cast<int>(*yearValue),
                                        .month = static_cast<unsigned>(_month.peek()),
                                        .timezoneOffsetMinutes = _options.timezoneOffsetMinutes};
    _outcome.set(std::nullopt);
    _submit.run(SubmitReport{.ledgerId = *book,
                             .kind = ReportKind::MonthlyStatement,
                             .params = glz::write_json(params).value_or(std::string{"{}"})});
}

StatementStatus StatementController::status() const {
    if (_submit.pending() || _job.get().has_value()) {
        return StatementStatus::Pending;
    }
    if (auto const& outcome = _outcome.get()) {
        return outcome->failed ? StatementStatus::Failed : StatementStatus::Done;
    }
    return _submit.error() != nullptr ? StatementStatus::Failed : StatementStatus::Idle;
}

std::string StatementController::statusText() const {
    switch (status()) {
        case StatementStatus::Idle:
            return "idle";
        case StatementStatus::Pending:
            return "pending";
        case StatementStatus::Done:
            return "done";
        case StatementStatus::Failed:
            return "failed";
        default:
            return "idle";
    }
}

std::vector<StatementLine> StatementController::lines() const {
    auto const& outcome = _outcome.get();
    return outcome && !outcome->failed ? outcome->lines : std::vector<StatementLine>{};
}

std::string StatementController::offsetText() const {
    return "Offset applied: " + std::to_string(_options.timezoneOffsetMinutes) + " minutes from UTC";
}

std::string StatementController::errorText() const {
    if (auto const& outcome = _outcome.get(); outcome && outcome->failed) {
        return outcome->message;
    }
    if (std::exception_ptr const error = _submit.error(); error != nullptr) {
        return morph::reactive::errorMessage(error);
    }
    return {};
}

void StatementController::settle() {
    auto const& reply = _status.value();
    std::exception_ptr const failure = _status.error();
    _wiring.runtime.untracked([&] {
        if (!_job.peek()) {
            return;
        }
        if (failure != nullptr) {
            finish(Outcome{.failed = true, .message = morph::reactive::errorMessage(failure), .lines = {}});
            return;
        }
        if (!reply || reply->status == ReportStatus::Pending) {
            return;
        }
        if (reply->status == ReportStatus::Failed) {
            finish(Outcome{.failed = true, .message = "report job failed", .lines = {}});
            return;
        }
        auto decoded = statementLines(reply->result.value_or(std::string{}));
        if (!decoded) {
            finish(Outcome{.failed = true, .message = "could not decode report body", .lines = {}});
            return;
        }
        finish(Outcome{.failed = false, .message = {}, .lines = std::move(*decoded)});
    });
}

void StatementController::finish(Outcome outcome) {
    // Clearing the job makes the status query idle, which is what stops the timed refresh from polling a job that
    // has settled.
    _wiring.runtime.batch([&] {
        _outcome.set(std::move(outcome));
        _job.set(std::nullopt);
    });
}

}  // namespace ledger::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_tests
./build/all/examples/ledger/ladder_ledger_tests "[ledger][client][report]"
```

Expected: PASS, 2 cases.

Mutation check: in `finish`, delete `_job.set(std::nullopt);`. Expected FAIL in "a statement is polled to Done…"
(`status()` stays Pending, or — once Done is reached through the outcome — `pollCount()` grows after the 10 s
advance). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/app/controllers/statement_controller.hpp \
        examples/ledger/app/controllers/statement_controller.cpp \
        examples/ledger/tests/client/test_statement_controller.cpp
git commit -m "wip(ledger): StatementController: the statement job polled by a Query with refreshEvery

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task L7: The multi-client stress case drives `AccountsController`s

Re-expresses `tests/test_multiclient.cpp` (spec 4 §7 rule 6: stress harnesses are unchanged but drive controllers):
N accounts controllers on one bridge store a seeded, weighted script of transfers at one book; every burst checks
the zero-sum invariant; the run checks convergence and that no controller reported an error.

**Files:**
- Modify (rewrite): `examples/ledger/tests/test_multiclient.cpp`
- Test: the same file

**Interfaces:**
- Consumes: `AccountsController::storeTransfer`, `busy`, `errorText` (Task L3); `BootController` (Task L2);
  `ledger::testing::LocalClient`, `seedLedger`, `openAccountDirect` (Task L2); `testkit/action_driver.hpp`
  (`SeededScript`), `testkit/convergence.hpp` (`pollUntilConverged`).
- Produces: nothing new.

- [ ] **Step 1: Write the failing test**

Replace the whole of `examples/ledger/tests/test_multiclient.cpp` with:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Rung 5's multi-client stress case (examples/TESTING.md, "Multi-client stress harness"). N accounts controllers
// on one bridge drive a seeded, weighted script of transfers at one book, and the run asserts the two invariants
// that must survive concurrency:
//
//   * every currency's legs still sum to exactly zero, and
//   * every client converges on the same ledger state.
//
// Scale comes from MORPH_LADDER_CLIENTS / MORPH_LADDER_ACTIONS and the seed from MORPH_STRESS_SEED (SeededScript
// prints it on failure), so a CI failure is reproducible by re-running with the same seed.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <ledger/models/ledger_model.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>
#include <vector>

#include "client/ledger_client_support.hpp"
#include "controllers/accounts_controller.hpp"
#include "controllers/boot_controller.hpp"
#include "testkit/action_driver.hpp"
#include "testkit/convergence.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::pollUntilConverged;
using morph::ladder::testkit::SeededScript;

[[nodiscard]] int envCount(char const* name, int fallback) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read on the test thread before any client starts work.
    char const* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    int const parsed = std::atoi(raw);
    return parsed > 0 ? parsed : fallback;
}

struct Move {
    std::int64_t amountMinor;
    bool reversed;
};

[[nodiscard]] ledger::GetLedgerResult readBook(ledger::LedgerId book) {
    morph::session::Context context;
    context.principal = "alice";
    morph::session::detail::ScopedContext const scope{context};
    ledger::LedgerModel model;
    return model.execute(ledger::GetLedger{.ledgerId = book});
}

}  // namespace

TEST_CASE("N clients storing concurrent transactions converge, legs always sum zero", "[ledger][stress]") {
    auto const nClients = static_cast<std::size_t>(envCount("MORPH_LADDER_CLIENTS", 4));
    int const nActions = envCount("MORPH_LADDER_ACTIONS", 40);
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Stress");
    auto const checking = ledger::testing::openAccountDirect(book, "Checking", ledger::AccountKind::Asset);
    auto const groceries = ledger::testing::openAccountDirect(book, "Groceries", ledger::AccountKind::Expense);
    ledger::testing::LocalClient client{"alice"};
    ledger::client::BootController boot{
        client.wiring(),
        ledger::client::BootOptions{.principal = "alice", .remote = false, .seed = false, .ledger = *book}};
    std::vector<std::unique_ptr<ledger::client::AccountsController>> clients;
    for (std::size_t i = 0; i < nClients; ++i) {
        clients.push_back(std::make_unique<ledger::client::AccountsController>(client.wiring(), boot));
    }

    SeededScript<Move> script{/*defaultSeed=*/20260819,
                              /*generators=*/
                              {{3, [] { return Move{.amountMinor = 500, .reversed = false}; }},
                               {1, [] { return Move{.amountMinor = 250, .reversed = true}; }}},
                              /*burstSize=*/10,
                              /*onBurst=*/
                              [&](std::vector<Move> const&) {
                                  auto const state = readBook(book);
                                  morph::math::Rational total = morph::math::Rational::zero(morph::math::DecimalPlaces{2});
                                  for (auto const& account : state.accounts) {
                                      total = total + account.balance;
                                  }
                                  CHECK(total.numerator == 0);
                              }};
    for (int i = 0; i < nActions; ++i) {
        Move const move = script.next();
        auto& controller = *clients.at(static_cast<std::size_t>(i) % clients.size());
        controller.storeTransfer(ledger::client::Transfer{.from = move.reversed ? groceries : checking,
                                                          .to = move.reversed ? checking : groceries,
                                                          .amountMinor = move.amountMinor,
                                                          .description = "stress"});
    }
    script.flushBurst();

    REQUIRE(client.settle(
        [&clients] { return std::ranges::none_of(clients, [](auto const& controller) { return controller->busy(); }); },
        std::chrono::seconds{30}));
    for (auto const& controller : clients) {
        CHECK(controller->errorText().empty());
    }

    auto const fingerprints = [&] {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < clients.size(); ++i) {
            std::string fingerprint;
            for (auto const& account : readBook(book).accounts) {
                fingerprint += std::to_string(account.balance.numerator) + "/" +
                               std::to_string(account.balance.denominator) + "@" +
                               std::to_string(account.balance.decimalPlaces.value) + ";";
            }
            out.push_back(std::move(fingerprint));
        }
        return out;
    };
    CHECK(pollUntilConverged(fingerprints, /*maxAttempts=*/20));
    auto const state = readBook(book);
    REQUIRE(state.accounts.size() == 2);
    INFO("checking=" << state.accounts.at(0).balance.numerator
                     << " groceries=" << state.accounts.at(1).balance.numerator);
    CHECK(state.accounts.at(0).balance.numerator != 0);
    CHECK(state.accounts.at(0).balance.numerator == -state.accounts.at(1).balance.numerator);
    INFO("MORPH_STRESS_SEED=" << script.seed());
    INFO("clients=" << clients.size() << " actions=" << nActions);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run the mutation first (Task L3 is in place): in `AccountsController::storeTransfer`, give the credit leg
`cents(transfer.amountMinor + 1)`. Then

```bash
cmake --build build/all --target ladder_ledger_tests
./build/all/examples/ledger/ladder_ledger_tests "[ledger][stress]"
```

Expected: FAIL — every store is refused by the model's zero-sum check, so `errorText()` is non-empty and the
balances stay zero. Restore `cents(transfer.amountMinor)`.

- [ ] **Step 3: Implement**

No production change.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
./build/all/examples/ledger/ladder_ledger_tests "[ledger][stress]"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/tests/test_multiclient.cpp
git commit -m "wip(ledger): the multi-client stress case drives AccountsControllers

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task L8: Ledger's `AppController` and views — four tabs

`Main.qml`'s `TabBar` + `StackLayout` becomes a `ui::Tabs` behind a `Switch` on the boot state (signing in → a
`Busy`; failed → the reason; ready → the tabs). Each tab is bindings over its controller; the hand-built inputs of
the QML views stay hand-built `TextInput`s and `Select`s (ledger never used the forms renderer), and the accounts
list becomes a `Table`.

**Files:**
- Create: `examples/ledger/app/app_controller.hpp`, `examples/ledger/app/app_controller.cpp`
- Create: `examples/ledger/app/views/ledger_views.hpp`, `examples/ledger/app/views/ledger_views.cpp`
- Test: `examples/ledger/tests/client/test_ledger_views.cpp`

**Interfaces:**
- Consumes: Tasks L2–L6; Part 2's `ui::tabs`, `Tab`, `table<RowT>`, `TableColumn`, `textInput`, `select`, `busy`,
  `grid`, `switchOn<E>`; `RecordingBackend::find`, `prop`, `edit`, `click`, `dump` (a `Tabs` page is mounted when
  its tab is first selected).
- Produces: `ledger::client::AppOptions{boot, statement}`, `AppController(examples::Wiring, AppOptions)` with `boot()`,
  `accounts()`, `budget()`, `rules()`, `statement()`, `tab()`, `selectTab(std::size_t)`; views `bootView`,
  `accountsTab(AccountsController&)`, `budgetsTab(BudgetController&)`, `rulesTab(RulesController&)`,
  `statementTab(StatementController&)`, `rootView(AppController&)`.

- [ ] **Step 1: Write the failing test**

Create `examples/ledger/tests/client/test_ledger_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// One RecordingBackend test per screen: the tree shows what the screen must, and its controls drive the controller.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>

#include "app_controller.hpp"
#include "ledger_client_support.hpp"
#include "testkit/db_fixture.hpp"
#include "views/ledger_views.hpp"

namespace {

using ledger::client::AppController;
using ledger::client::AppOptions;
using ledger::client::BootOptions;
using ledger::testing::LocalClient;
using morph::ladder::testkit::DbFixture;
using morph::ui::testing::RecordingBackend;

[[nodiscard]] bool shows(RecordingBackend const& backend, std::string const& text) {
    return backend.dump().find(text) != std::string::npos;
}

[[nodiscard]] int byLabel(RecordingBackend const& backend, std::string const& label) {
    auto const found = backend.find("Button", "label", label);
    REQUIRE(found.has_value());
    return *found;
}

[[nodiscard]] int byPlaceholder(RecordingBackend const& backend, std::string const& placeholder) {
    auto const found = backend.find("TextInput", "placeholder", placeholder);
    REQUIRE(found.has_value());
    return *found;
}

// Clicks the button labelled @p label once it is enabled: a button's `enabled` follows its controller's `can…()`
// one flush after an input changes, and the backend ignores a click on a disabled widget.
void clickWhenEnabled(LocalClient& client, RecordingBackend& backend, std::string const& label) {
    int const target = byLabel(backend, label);
    REQUIRE(client.settle([&backend, target] { return backend.prop(target, "enabled") != "false"; }));
    backend.click(target);
}

[[nodiscard]] AppOptions optionsFor(ledger::LedgerId book, bool seed = false) {
    return AppOptions{.boot = BootOptions{.principal = "alice", .remote = false, .seed = seed, .ledger = *book},
                      .statement = {}};
}

}  // namespace

TEST_CASE("ledger view: the root shows boot progress, then the four tabs", "[ledger][view]") {
    DbFixture const fixture;
    LocalClient client;
    AppController app{client.wiring(), optionsFor(ledger::LedgerId{1}, /*seed=*/true)};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, ledger::client::rootView(app)};
    REQUIRE(client.settle([&] { return shows(backend, "Refresh"); }));
    // The four labels are the Tabs widget's own; only the selected (accounts) page is mounted until another is chosen.
    CHECK(backend.find("Tabs", "tabs", "[Accounts,Budgets,Rules,Statement]").has_value());
    CHECK_FALSE(backend.find("Button", "label", "Create category").has_value());
}

TEST_CASE("ledger view: the accounts tab opens an account and stores a transfer", "[ledger][view]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    auto const checking = ledger::testing::openAccountDirect(book, "Checking", ledger::AccountKind::Asset);
    LocalClient client{"alice"};
    AppController app{client.wiring(), optionsFor(book)};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, ledger::client::accountsTab(app.accounts())};
    REQUIRE(client.settle([&] { return shows(backend, "Checking"); }));

    backend.edit(byPlaceholder(backend, "New account name"), "Groceries");
    app.accounts().setNewKind(ledger::AccountKind::Expense);
    clickWhenEnabled(client, backend, "Open account");
    REQUIRE(client.settle([&] { return app.accounts().accounts().size() == 2; }));
    REQUIRE(client.settle([&] { return shows(backend, "Groceries"); }));

    app.accounts().setFrom(*checking);
    app.accounts().setTo(app.accounts().accounts().back().id);
    backend.edit(byPlaceholder(backend, "Amount (cents)"), "5000");
    clickWhenEnabled(client, backend, "Store");
    REQUIRE(client.settle([&] { return shows(backend, "-50"); }));
}

TEST_CASE("ledger view: the budgets tab creates a category and shows its id", "[ledger][view]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    AppController app{client.wiring(), optionsFor(book)};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, ledger::client::budgetsTab(app.budget())};
    REQUIRE(client.settle([&] { return backend.find("Button", "label", "Create category").has_value(); }));

    backend.edit(byPlaceholder(backend, "Category name"), "Groceries");
    clickWhenEnabled(client, backend, "Create category");

    REQUIRE(client.settle([&] { return shows(backend, "Category created (id "); }));
}

TEST_CASE("ledger view: the rules tab creates a rule and shows it as the last rule", "[ledger][view]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    AppController app{client.wiring(), optionsFor(book)};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, ledger::client::rulesTab(app.rules())};
    REQUIRE(client.settle([&] { return backend.find("Button", "label", "Create rule").has_value(); }));

    backend.edit(byPlaceholder(backend, "Description contains…"), "COFFEE");
    clickWhenEnabled(client, backend, "Create rule");

    REQUIRE(client.settle([&] { return shows(backend, "Rule created"); }));
}

TEST_CASE("ledger view: the statement tab's Request submits a statement", "[ledger][view]") {
    DbFixture const fixture;
    auto const book = ledger::testing::seedLedger("Personal");
    LocalClient client{"alice"};
    AppController app{client.wiring(), optionsFor(book)};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, ledger::client::statementTab(app.statement())};
    REQUIRE(client.settle([&] { return shows(backend, "Monthly statement") && shows(backend, "idle"); }));

    clickWhenEnabled(client, backend, "Request");

    REQUIRE(client.settle([&] { return app.statement().pollCount() >= 1; }));
    CHECK(shows(backend, "pending"));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_ledger_tests`
Expected: compile error, `'app_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/ledger/app/app_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/reactive/signal.hpp>

#include "app/wiring.hpp"
#include "controllers/accounts_controller.hpp"
#include "controllers/boot_controller.hpp"
#include "controllers/budget_controller.hpp"
#include "controllers/rules_controller.hpp"
#include "controllers/statement_controller.hpp"

namespace ledger::client {

/// @brief What the ledger app is configured with besides its wiring.
struct AppOptions {
    /// @brief Who runs it, in which mode, on which book.
    BootOptions boot;
    /// @brief The statement's display zone and poll period.
    StatementOptions statement;
};

/// @brief The ledger client: startup and the four tabs' controllers.
class AppController {
public:
    /// @param wiring The runtime, scheduler, bridge and owner every controller uses.
    /// @param options Boot and statement options.
    AppController(morph::examples::Wiring wiring, AppOptions options);

    /// @brief The boot controller. @return It.
    [[nodiscard]] BootController& boot() noexcept { return _boot; }
    /// @brief The accounts tab's controller. @return It.
    [[nodiscard]] AccountsController& accounts() noexcept { return _accounts; }
    /// @brief The budgets tab's controller. @return It.
    [[nodiscard]] BudgetController& budget() noexcept { return _budget; }
    /// @brief The rules tab's controller. @return It.
    [[nodiscard]] RulesController& rules() noexcept { return _rules; }
    /// @brief The statement tab's controller. @return It.
    [[nodiscard]] StatementController& statement() noexcept { return _statement; }
    /// @brief The selected tab. Tracked. @return Its index.
    [[nodiscard]] std::size_t tab() const { return _tab.get(); }
    /// @brief Selects a tab. @param index Its index.
    void selectTab(std::size_t index) { _tab.set(index); }

private:
    BootController _boot;
    AccountsController _accounts;
    BudgetController _budget;
    RulesController _rules;
    StatementController _statement;
    morph::reactive::Signal<std::size_t> _tab;
};

}  // namespace ledger::client
```

Create `examples/ledger/app/app_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "app_controller.hpp"

#include <utility>

namespace ledger::client {

AppController::AppController(morph::examples::Wiring wiring, AppOptions options)
    : _boot{wiring, std::move(options.boot)},
      _accounts{wiring, _boot},
      _budget{wiring, _boot},
      _rules{wiring, _boot},
      _statement{wiring, _boot, options.statement},
      _tab{wiring.runtime, std::size_t{0}} {}

}  // namespace ledger::client
```

Create `examples/ledger/app/views/ledger_views.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "app_controller.hpp"

namespace ledger::client {

/// @brief The accounts tab. @param accounts Its controller. @return The tab's content.
[[nodiscard]] morph::ui::Node accountsTab(AccountsController& accounts);
/// @brief The budgets tab. @param budget Its controller. @return The tab's content.
[[nodiscard]] morph::ui::Node budgetsTab(BudgetController& budget);
/// @brief The rules tab. @param rules Its controller. @return The tab's content.
[[nodiscard]] morph::ui::Node rulesTab(RulesController& rules);
/// @brief The statement tab. @param statement Its controller. @return The tab's content.
[[nodiscard]] morph::ui::Node statementTab(StatementController& statement);
/// @brief The whole app: boot progress, its failure, or the four tabs.
/// @param app The app; it outlives the mounted view.
/// @return The root node.
[[nodiscard]] morph::ui::Node rootView(AppController& app);

}  // namespace ledger::client
```

Create `examples/ledger/app/views/ledger_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/ledger_views.hpp"

#include <cstdint>
#include <functional>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ledger::client {

namespace {

namespace ui = morph::ui;
using morph::reactive::Signal;

template <typename Controller, typename Input>
ui::Node field(Controller& controller, Input which, std::string placeholder) {
    return ui::textInput({.value = [&controller, which] { return controller.input(which); },
                          .onChange = [&controller, which](std::string text) { controller.setInput(which, std::move(text)); },
                          .placeholder = std::move(placeholder),
                          .common = {.layout = {.width = ui::Sizing::stretch()}}});
}

ui::Node stretchText(std::function<std::string()> text, ui::TextRole role = ui::TextRole::Normal) {
    return ui::text({.text = std::move(text), .role = role, .common = {.layout = {.width = ui::Sizing::stretch()}}});
}

std::vector<ui::SelectOption> accountOptions(AccountsController const& accounts) {
    std::vector<ui::SelectOption> options;
    for (AccountRow const& row : accounts.accounts()) {
        options.push_back(ui::SelectOption{.key = ui::Key{row.id}, .label = row.name});
    }
    return options;
}

std::optional<ui::Key> keyOf(std::optional<std::int64_t> const& accountId) {
    if (!accountId) {
        return std::nullopt;
    }
    return ui::Key{*accountId};
}

}  // namespace

ui::Node accountsTab(AccountsController& accounts) {
    std::vector<ui::SelectOption> kinds;
    for (std::string const& name : kindNames()) {
        kinds.push_back(ui::SelectOption{.key = ui::Key{name}, .label = name});
    }
    return ui::column({
        .children =
            {
                ui::row({.children = {ui::text({.text = "Accounts", .role = ui::TextRole::Heading}),
                                      ui::busy({.active = [&accounts] { return accounts.busy(); }, .label = ""}),
                                      ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                      ui::button({.label = "Refresh", .onClick = [&accounts] { accounts.refresh(); }})},
                         .gap = 1}),
                ui::table<AccountRow>(
                    {{.label = "Name", .width = ui::Sizing::stretch()},
                     {.label = "Kind"},
                     {.label = "Currency"},
                     {.label = "Balance"}},
                    [&accounts] { return accounts.accounts(); }, [](AccountRow const& row) { return ui::Key{row.id}; },
                    [](Signal<AccountRow> const& row) {
                        return std::vector<ui::Node>{
                            ui::text({.text = [&row] { return row.get().name; }}),
                            ui::text({.text = [&row] { return row.get().kind; }, .role = ui::TextRole::Muted}),
                            ui::text({.text = [&row] { return row.get().currency; }, .role = ui::TextRole::Muted}),
                            ui::text({.text = [&row] { return row.get().balanceText; }})};
                    }),
                ui::row({.children = {field(accounts, AccountsInput::NewName, "New account name"),
                                      ui::select({.options = std::move(kinds),
                                                  .selected = [&accounts]() -> std::optional<ui::Key> {
                                                      return ui::Key{kindText(accounts.newKind())};
                                                  },
                                                  .onSelect =
                                                      [&accounts](ui::Key const& key) {
                                                          if (auto const* const name = std::get_if<std::string>(&key);
                                                              name != nullptr) {
                                                              accounts.setNewKind(kindFromText(*name));
                                                          }
                                                      }}),
                                      ui::button({.label = "Open account",
                                                  .onClick = [&accounts] { accounts.openAccount(); },
                                                  .common = {.enabled = [&accounts] { return accounts.canOpenAccount(); }}})},
                         .gap = 1}),
                ui::row({.children = {ui::select({.options = [&accounts] { return accountOptions(accounts); },
                                                  .selected = [&accounts] { return keyOf(accounts.from()); },
                                                  .onSelect =
                                                      [&accounts](ui::Key const& key) {
                                                          if (auto const* const account = std::get_if<std::int64_t>(&key);
                                                              account != nullptr) {
                                                              accounts.setFrom(*account);
                                                          }
                                                      }}),
                                      ui::select({.options = [&accounts] { return accountOptions(accounts); },
                                                  .selected = [&accounts] { return keyOf(accounts.to()); },
                                                  .onSelect =
                                                      [&accounts](ui::Key const& key) {
                                                          if (auto const* const account = std::get_if<std::int64_t>(&key);
                                                              account != nullptr) {
                                                              accounts.setTo(*account);
                                                          }
                                                      }}),
                                      field(accounts, AccountsInput::Amount, "Amount (cents)"),
                                      field(accounts, AccountsInput::Description, "Description"),
                                      ui::button({.label = "Store",
                                                  .onClick = [&accounts] { accounts.store(); },
                                                  .common = {.enabled = [&accounts] { return accounts.canStore(); }}})},
                         .gap = 1}),
                ui::row({.children = {ui::text({.text = "Entries", .role = ui::TextRole::Heading}),
                                      field(accounts, AccountsInput::Month, "Month (YYYY-MM)"),
                                      ui::button({.label = "List",
                                                  .onClick = [&accounts] { accounts.list(); },
                                                  .common = {.enabled = [&accounts] { return accounts.canList(); }}})},
                         .gap = 1}),
                ui::forEach<EntryRow>(
                    [&accounts] { return accounts.entries(); }, [](EntryRow const& entry) { return ui::Key{entry.id}; },
                    [&accounts](Signal<EntryRow> const& entry) {
                        return ui::row(
                            {.children = {ui::text({.text = [&entry] { return entry.get().idText; },
                                                    .role = ui::TextRole::Muted}),
                                          stretchText([&entry] { return entry.get().description; }),
                                          ui::text({.text = [&entry] { return entry.get().dateText; },
                                                    .role = ui::TextRole::Muted}),
                                          ui::button({.label = "Undo",
                                                      .onClick = [&accounts, &entry] { accounts.undo(entry.peek().id); }})},
                             .gap = 1});
                    }),
                ui::text({.text = [&accounts] { return accounts.errorText(); }, .role = ui::TextRole::Error}),
            },
        .gap = 1,
    });
}

ui::Node budgetsTab(BudgetController& budget) {
    return ui::column({
        .children =
            {
                ui::row({.children = {ui::text({.text = "Budgets", .role = ui::TextRole::Heading}),
                                      ui::busy({.active = [&budget] { return budget.busy(); }, .label = ""})},
                         .gap = 1}),
                ui::text({.text = [&budget] { return budget.statusText(); }, .role = ui::TextRole::Muted}),
                ui::row({.children = {field(budget, BudgetInput::CategoryName, "Category name"),
                                      ui::button({.label = "Create category",
                                                  .onClick = [&budget] { budget.createCategory(); },
                                                  .common = {.enabled = [&budget] { return budget.canCreateCategory(); }}}),
                                      ui::text({.text = [&budget] { return budget.lastCategoryText(); },
                                                .role = ui::TextRole::Muted})},
                         .gap = 1}),
                ui::row({.children = {field(budget, BudgetInput::LinkAccount, "Account id"),
                                      field(budget, BudgetInput::LinkCategory, "Category id"),
                                      ui::button({.label = "Link account to category",
                                                  .onClick = [&budget] { budget.linkAccount(); },
                                                  .common = {.enabled = [&budget] { return budget.canLink(); }}})},
                         .gap = 1}),
                ui::row({.children = {field(budget, BudgetInput::BudgetName, "Budget name"),
                                      field(budget, BudgetInput::BudgetCategory, "Budget category id"),
                                      ui::button({.label = "Create budget",
                                                  .onClick = [&budget] { budget.createBudget(); },
                                                  .common = {.enabled = [&budget] { return budget.canCreateBudget(); }}}),
                                      ui::text({.text = [&budget] { return budget.lastBudgetText(); },
                                                .role = ui::TextRole::Muted})},
                         .gap = 1}),
                ui::row({.children = {field(budget, BudgetInput::LimitBudget, "Budget id"),
                                      field(budget, BudgetInput::LimitMonth, "Month (YYYY-MM)"),
                                      field(budget, BudgetInput::LimitMinor, "Limit (cents)"),
                                      ui::button({.label = "Set limit",
                                                  .onClick = [&budget] { budget.setLimit(); },
                                                  .common = {.enabled = [&budget] { return budget.canSetLimit(); }}}),
                                      ui::button({.label = "Report",
                                                  .onClick = [&budget] { budget.report(); },
                                                  .common = {.enabled = [&budget] { return budget.canReport(); }}})},
                         .gap = 1}),
                ui::grid({.columns = 2,
                          .cells = {{.node = ui::text({.text = "Limit"})},
                                    {.node = ui::text({.text = [&budget] { return budget.limitText(); }})},
                                    {.node = ui::text({.text = "Spent"})},
                                    {.node = ui::text({.text = [&budget] { return budget.spentText(); }})},
                                    {.node = ui::text({.text = "Currency"})},
                                    {.node = ui::text({.text = [&budget] { return budget.currencyText(); }})}},
                          .gap = 1}),
                ui::text({.text = [&budget] { return budget.errorText(); }, .role = ui::TextRole::Error}),
            },
        .gap = 1,
    });
}

ui::Node rulesTab(RulesController& rules) {
    return ui::column({
        .children =
            {
                ui::row({.children = {ui::text({.text = "Categorisation rules", .role = ui::TextRole::Heading}),
                                      ui::busy({.active = [&rules] { return rules.busy(); }, .label = ""})},
                         .gap = 1}),
                ui::text({.text = "Editing a rule bumps its version. Transactions already categorised keep the "
                                  "version that categorised them — an edit never rewrites history.",
                          .role = ui::TextRole::Muted}),
                ui::text({.text = [&rules] { return rules.statusText(); }, .role = ui::TextRole::Muted}),
                ui::row({.children = {field(rules, RuleInput::MatchText, "Description contains…"),
                                      field(rules, RuleInput::CategoryId, "Set category id"),
                                      ui::button({.label = "Create rule",
                                                  .onClick = [&rules] { rules.create(); },
                                                  .common = {.enabled = [&rules] { return rules.canCreate(); }}})},
                         .gap = 1}),
                ui::row({.children = {field(rules, RuleInput::EditRuleId, "Rule id"),
                                      field(rules, RuleInput::EditMatch, "New match text"),
                                      field(rules, RuleInput::EditCategory, "New category id"),
                                      ui::button({.label = "Update rule",
                                                  .onClick = [&rules] { rules.update(); },
                                                  .common = {.enabled = [&rules] { return rules.canUpdate(); }}})},
                         .gap = 1}),
                ui::panel({.title = "Last rule",
                           .padding = 1,
                           .child = ui::grid({.columns = 2,
                                              .cells = {{.node = ui::text({.text = "id"})},
                                                        {.node = ui::text({.text = [&rules] { return rules.lastRuleId(); }})},
                                                        {.node = ui::text({.text = "match"})},
                                                        {.node = ui::text({.text = [&rules] { return rules.lastRuleMatch(); }})},
                                                        {.node = ui::text({.text = "version"})},
                                                        {.node = ui::text({.text = [&rules] { return rules.lastRuleVersion(); },
                                                                           .role = ui::TextRole::Heading})}},
                                              .gap = 1})}),
                ui::text({.text = [&rules] { return rules.errorText(); }, .role = ui::TextRole::Error}),
            },
        .gap = 1,
    });
}

ui::Node statementTab(StatementController& statement) {
    std::vector<ui::SelectOption> months;
    for (std::int64_t month = 1; month <= 12; ++month) {
        months.push_back(ui::SelectOption{.key = ui::Key{month}, .label = std::to_string(month)});
    }
    return ui::column({
        .children =
            {
                ui::text({.text = "Monthly statement", .role = ui::TextRole::Heading}),
                ui::row({.children = {ui::textInput({.value = [&statement] { return statement.year(); },
                                                     .onChange = [&statement](std::string text) { statement.setYear(std::move(text)); },
                                                     .placeholder = "Year"}),
                                      ui::select({.options = std::move(months),
                                                  .selected = [&statement]() -> std::optional<ui::Key> {
                                                      return ui::Key{std::int64_t{statement.month()}};
                                                  },
                                                  .onSelect =
                                                      [&statement](ui::Key const& key) {
                                                          if (auto const* const month = std::get_if<std::int64_t>(&key);
                                                              month != nullptr) {
                                                              statement.setMonth(static_cast<int>(*month));
                                                          }
                                                      }}),
                                      ui::button({.label = "Request",
                                                  .onClick = [&statement] { statement.request(); },
                                                  .common = {.enabled = [&statement] { return statement.canRequest(); }}}),
                                      ui::text({.text = [&statement] { return statement.statusText(); },
                                                .role = ui::TextRole::Heading}),
                                      ui::busy({.active = [&statement] { return statement.pending(); }, .label = ""})},
                         .gap = 1}),
                ui::text({.text = statement.offsetText(), .role = ui::TextRole::Muted}),
                ui::forEach<StatementLine>(
                    [&statement] { return statement.lines(); },
                    [](StatementLine const& line) { return ui::Key{line.currency}; },
                    [](Signal<StatementLine> const& line) {
                        return ui::row({.children = {stretchText([&line] { return line.get().currency; }),
                                                     ui::text({.text = [&line] { return line.get().countText; },
                                                               .role = ui::TextRole::Muted}),
                                                     ui::text({.text = [&line] { return line.get().amountText; }})},
                                        .gap = 1});
                    }),
                ui::text({.text = [&statement] { return statement.errorText(); }, .role = ui::TextRole::Error}),
            },
        .gap = 1,
    });
}

ui::Node rootView(AppController& app) {
    BootController& boot = app.boot();
    return ui::switchOn<BootState>(
        [&boot] { return boot.state(); },
        {{BootState::SigningIn, ui::busy({.active = true, .label = [&boot] { return boot.statusText(); }})},
         {BootState::Failed, ui::text({.text = [&boot] { return boot.statusText(); }, .role = ui::TextRole::Error})},
         {BootState::Ready,
          ui::tabs({.tabs = {{.label = "Accounts", .node = accountsTab(app.accounts())},
                             {.label = "Budgets", .node = budgetsTab(app.budget())},
                             {.label = "Rules", .node = rulesTab(app.rules())},
                             {.label = "Statement", .node = statementTab(app.statement())}},
                    .selected = [&app] { return app.tab(); },
                    .onSelect = [&app](std::size_t index) { app.selectTab(index); }})}});
}

}  // namespace ledger::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_tests
./build/all/examples/ledger/ladder_ledger_tests "[ledger][view]"
git grep -n -E '#include <Q|morph/qt|morph/tui|morph/qt_quick' -- examples/ledger/app; test $? -eq 1 && echo ok-app
git grep -n 'morph/ui/' -- examples/ledger/app/controllers; test $? -eq 1 && echo ok-controllers
```

Expected: PASS (5 cases); `ok-app`, `ok-controllers`.

Mutation check: in `accountsTab`, bind the Store button's `onClick` to `accounts.openAccount()`. Expected FAIL in
"the accounts tab opens an account and stores a transfer" ("-50" never shows). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/app/app_controller.hpp examples/ledger/app/app_controller.cpp examples/ledger/app/views \
        examples/ledger/tests/client/test_ledger_views.cpp
git commit -m "wip(ledger): AppController and the four tab views

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task L9: Ledger's application, binary and frontend smoke tests

`ledger::client::makeApplication(ctx, env)` connects, runs as `--user` (default `demo`), and opens book 1 — the id
`Main.qml` hard-coded — or, with `--seed`, a new "Personal" book. The statement's display zone is this machine's
offset from UTC, read when the application is built (`localOffsetMinutes()`), as `ReportView.qml` took it from the
JavaScript `Date`. Neither is a command-line knob, so `ui/main.cpp` is exactly spec 4 §3's composition root.

**Files:**
- Create: `examples/ledger/app/ledger_application.hpp`, `examples/ledger/app/ledger_application.cpp`
- Create: `examples/ledger/ui/main.cpp`
- Test: `examples/ledger/tests/smoke/test_ledger_frontends.cpp`

**Interfaces:**
- Consumes: as Task K13 (Part 6's `AppEnvironment`, `connect`, `LocalSetup`, `Connection`, `Wiring`; Part 2's
  frontend seam; Parts 3–4's `frontendOption`s; `runFrontendSmoke`); `AppController`, `AppOptions`, `BootOptions`,
  `StatementOptions`, `rootView`, `localOffsetMinutes` (Tasks L2, L6, L8); `ledger::db::setup`;
  `Bridge::setExecuteDeadline` (`morph/core/bridge.hpp:1020`).
- Produces: `ledger::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&) ->
  std::unique_ptr<ui::Application>`; the `ledger` binary.

- [ ] **Step 1: Write the failing test**

Create `examples/ledger/tests/smoke/test_ledger_frontends.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The application mounts and quits on every frontend this build has (spec 4 §7 rule 5). This file is
// ladder_ledger_smoke_tests, a binary of its own: its main owns no Qt application object.

#if MORPH_EXAMPLE_HAS_TUI || MORPH_EXAMPLE_HAS_QT_QUICK

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"
#include "ledger_application.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/frontend_smoke.hpp"

namespace {

using morph::examples::testing::SmokeFrontend;

void smoke(SmokeFrontend frontend) {
    morph::ladder::testkit::DbFixture const fixture;
    morph::examples::AppEnvironment env;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read on the test thread before the app starts any thread.
    env.db = morph::ladder::testkit::DbFixture::computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"));
    env.user = "alice";
    env.seed = true;
    morph::examples::testing::runFrontendSmoke(
        [&env](morph::ui::AppContext& ctx) { return ledger::client::makeApplication(ctx, env); }, frontend);
}

}  // namespace

#if MORPH_EXAMPLE_HAS_TUI
TEST_CASE("ledger mounts and quits on the TUI", "[ledger][smoke]") { smoke(SmokeFrontend::Tui); }
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
TEST_CASE("ledger mounts and quits on Qt Quick", "[ledger][smoke]") { smoke(SmokeFrontend::QtQuick); }
#endif

#endif
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_ledger_smoke_tests`.
Expected: compile error, `'ledger_application.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/ledger/app/ledger_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"

namespace ledger::client {

/// @brief Builds the ledger application on a frontend's context: connects (local or `--server`), runs as `--user`
///        (default `demo`), and opens book 1 or, with `--seed`, a new "Personal" book. Statements draw their month
///        boundaries in this machine's offset from UTC.
/// @param ctx The frontend's context. Borrowed: it outlives the application.
/// @param env `--server`, `--db`, `--user`, `--seed`.
/// @return The application.
/// @throws morph::examples::TransportError when `--server` is given and this frontend has no transport for it.
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env);

}  // namespace ledger::client
```

Create `examples/ledger/app/ledger_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger_application.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "app/transport.hpp"
#include "app/wiring.hpp"
#include "app_controller.hpp"
#include "ledger/db/database.hpp"
#include "support/ledger_format.hpp"
#include "views/ledger_views.hpp"

namespace ledger::client {

namespace {

constexpr std::string_view kDefaultDatabase = "DRIVER=SQLite3;Database=ledger.db;Timeout=5000";
constexpr std::string_view kDevPrincipal = "demo";
// The book opened without --seed: the id the old client hard-coded.
constexpr std::int64_t kDefaultBook = 1;
// A call that never answers fails after this long, so a statement poll cannot leave the tab pending forever.
constexpr std::chrono::milliseconds kExecuteDeadline{5000};
constexpr std::size_t kLocalWorkers = 4;

[[nodiscard]] morph::examples::AppEnvironment withDatabase(morph::examples::AppEnvironment env) {
    if (env.db.empty()) {
        env.db = std::string{kDefaultDatabase};
    }
    return env;
}

[[nodiscard]] std::unique_ptr<morph::examples::Connection> withDeadline(
    std::unique_ptr<morph::examples::Connection> connection) {
    connection->bridge().setExecuteDeadline(kExecuteDeadline);
    return connection;
}

class LedgerApplication final : public morph::ui::Application {
public:
    LedgerApplication(morph::ui::AppContext& ctx, morph::examples::AppEnvironment const& env)
        : _connection{withDeadline(morph::examples::connect(
              ctx, withDatabase(env),
              morph::examples::LocalSetup{.setupDatabase = [](std::string const& database) { db::setup(database); },
                                          .workers = kLocalWorkers}))},
          _app{morph::examples::Wiring{.runtime = ctx.runtime(),
                                       .scheduler = ctx.scheduler(),
                                       .bridge = _connection->bridge(),
                                       .callbacks = _connection->callbacks()},
               AppOptions{.boot = BootOptions{.principal = env.user.empty() ? std::string{kDevPrincipal} : env.user,
                                              .remote = env.server.has_value(),
                                              .seed = env.seed,
                                              .ledger = kDefaultBook},
                          .statement = StatementOptions{.timezoneOffsetMinutes = localOffsetMinutes(),
                                                        .pollEvery = std::chrono::milliseconds{2000}}}} {}

    [[nodiscard]] morph::ui::Node view() override { return rootView(_app); }

private:
    std::unique_ptr<morph::examples::Connection> _connection;
    AppController _app;
};

}  // namespace

std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                        morph::examples::AppEnvironment const& env) {
    return std::make_unique<LedgerApplication>(ctx, env);
}

}  // namespace ledger::client
```

Create `examples/ledger/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The ledger client: picks the Qt Quick or terminal frontend at runtime (--ui=qt|tui, MORPH_UI, or the first that
// can run here) and hands it the application.
//
//   ledger [--ui=qt|tui] [--server ws://host:port] [--db <odbc>] [--user <name>] [--seed]

#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <vector>

#include "app/app_environment.hpp"
#include "ledger_application.hpp"

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#endif
#if MORPH_EXAMPLE_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

int main(int argc, char** argv) {
    try {
        auto const env = morph::examples::AppEnvironment::fromArgs(argc, argv);
        std::vector<morph::ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
        built.push_back(morph::qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
        built.push_back(morph::tui::frontendOption());
#endif
        auto const frontend = morph::ui::selectFrontend(built, argc, argv);
        return frontend->run([&env](morph::ui::AppContext& ctx) { return ledger::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "ledger: " << error.what() << '\n';
        return 1;
    }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_smoke_tests ledger
./build/all/examples/ledger/ladder_ledger_smoke_tests
```

Expected: both targets build; 2 smoke cases pass.

Mutation check: make `makeApplication` begin with `throw std::runtime_error{"mutation"};`. Expected FAIL in both smoke
cases. Restore it.

By hand, once: `./build/all/examples/ledger/ledger --ui=tui --seed --db "DRIVER=SQLite3;Database=/tmp/ledger-manual.db"`
— open two accounts, store a transfer, switch tabs, Ctrl+C; the same with `--ui=qt`. Report what you saw.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/app/ledger_application.hpp examples/ledger/app/ledger_application.cpp examples/ledger/ui \
        examples/ledger/tests/smoke/test_ledger_frontends.cpp
git commit -m "wip(ledger): the application and one binary for both frontends

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task L10: Retire ledger's QML stack; the README's client section

| Deleted test file | What it pinned | Where that claim lives now |
|---|---|---|
| `test_ledger_presenter.cpp` | GetLedger listed; OpenAccount carries the account; a refusal is relayed | `test_accounts_controller.cpp` ("a fresh book is empty…", "a model refusal surfaces on errorText") |
| `test_ledger_qml_bridge.cpp` | accounts as maps; balances exact, never float; refusal on `lastError` | `test_accounts_controller.cpp` ("a transfer carries balances exactly…"), `test_ledger_format.cpp` |
| `test_budget_presenter.cpp`, `test_budget_qml_bridge.cpp` | budget created; a month's limit and spend exact; refusal relayed | `test_budget_controller.cpp` |
| `test_rule_presenter.cpp` | create, update with the bumped version; refusal | `test_ledger_rules_controller.cpp` |
| `test_report_presenter.cpp` | submit → poll → done, exact lines; a second month | `test_statement_controller.cpp` ("a statement is polled to Done…") |
| `test_report_job_poller.cpp` | Done once then no more polls; Failed once; a dispatch error is terminal; a null error; a destroyed poller suppresses its reply | `test_statement_controller.cpp` (Done stops the poll; a refused submission fails and polls nothing); terminal-on-error and destroyed-in-flight are `reactive::Query`'s own contract (Part 1's query tests: an error is kept until the next success, a destroyed query gates replies), so they are not re-tested per app |
| `test_ledger_qml_surface.cpp` | bridges expose exactly what QML binds | by construction: views call controller members |
| `test_gui_qml_smoke.cpp` | each QML screen loads | `tests/smoke/test_ledger_frontends.cpp` and `test_ledger_views.cpp` (one per tab) |

**Files:**
- Delete: `examples/ledger/gui/`, `examples/ledger/gui_lib/`, and in `examples/ledger/tests/`:
  `test_ledger_presenter.cpp`, `test_ledger_qml_bridge.cpp`, `test_budget_presenter.cpp`,
  `test_budget_qml_bridge.cpp`, `test_rule_presenter.cpp`, `test_report_presenter.cpp`,
  `test_report_job_poller.cpp`, `test_ledger_qml_surface.cpp`, `test_gui_qml_smoke.cpp`
- Modify: `examples/ledger/CMakeLists.txt` (whole file, below), `examples/ledger/README.md`, `codecov.yml`, and the
  comments listed in Step 3
- Test: the whole ledger suite, plus the check below

**Interfaces:** none new.

- [ ] **Step 1: Write the failing check**

```bash
test ! -e examples/ledger/gui && test ! -e examples/ledger/gui_lib && \
  { git grep -n -E 'LedgerQmlBridge|LedgerPresenter|ReportJobPoller|ledger/gui_lib|ledger/gui/|\.qml' -- \
      examples/ledger codecov.yml; test $? -eq 1; } && echo "ledger's QML stack is gone"
```

- [ ] **Step 2: Run it to verify it fails**

Expected: no "gone" line (directories exist; `README.md` and `codecov.yml` match).

- [ ] **Step 3: Implement**

```bash
git rm -r -q examples/ledger/gui examples/ledger/gui_lib
git rm -q examples/ledger/tests/test_ledger_presenter.cpp examples/ledger/tests/test_ledger_qml_bridge.cpp \
          examples/ledger/tests/test_budget_presenter.cpp examples/ledger/tests/test_budget_qml_bridge.cpp \
          examples/ledger/tests/test_rule_presenter.cpp examples/ledger/tests/test_report_presenter.cpp \
          examples/ledger/tests/test_report_job_poller.cpp examples/ledger/tests/test_ledger_qml_surface.cpp \
          examples/ledger/tests/test_gui_qml_smoke.cpp
```

Replace the whole of `examples/ledger/CMakeLists.txt` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# ledger — rung 5 of the application ladder (examples/ledger/README.md).
# morph_add_rung() wires the standard targets: the domain library, the client app library (app/), the ledger
# binary (ui/main.cpp), the server with its report runner (src/server/app/, ladder_ledger_server_app), the tests
# and the frontend smoke tests (tests/smoke/). This file adds what is particular to this rung.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME ledger)

# morph_add_rung() globs src/models, src/db and src/app into ladder_ledger_lib; Login's validation lives beside
# them in src/dto.
if(TARGET ladder_ledger_lib)
    target_sources(ladder_ledger_lib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

```

`examples/ledger/README.md`:

1. Insert this section immediately before `## morph subsystems exercised`:

```markdown
## The client

One binary, `ledger` (`ui/main.cpp`), runs on Qt Quick or in a terminal: `--ui=qt|tui`, else `MORPH_UI`, else the
first frontend that can run here. Locally it opens `--db` (default `ledger.db`) as `--user` (default `demo`); with
`--server ws://host:port` it signs in to `ladder_ledger_server` with `Login` as that user. It works on book 1;
`--seed` creates a "Personal" book for the user and opens that instead.

`app/` is `ladder_ledger_app`, which links no toolkit: `BootController` (the session and the book),
`AccountsController`, `BudgetController`, `RulesController` and `StatementController` — the statement job is a
`Query` refreshed on the frontend's scheduler, which goes idle, and so stops polling, once the job settles — and one
view per tab. Statement jobs are run by the server's report runner (`src/server/app/app.cpp`), so a local
client's statement stays pending: run the server for statements.

**What looks different from the old Qt Quick screens.** Behaviour is kept; the layout is each frontend's own. The
accounts list is a table; a transfer's two accounts are picked from the book's accounts instead of typed as ids;
the statement's year is a text field and its month a list, instead of spin boxes.
```

2. Replace "`LedgerView.qml`'s Undo control now takes its id from a listed row instead of from a text field." with
   "The accounts tab's Undo control takes its id from a listed row."
3. Replace "each with model, presenter, QML bridge and tests. The desktop client wiring all four bridges is in
   `gui/`." with "each with model, client controller and tests; the client is `app/` with `ui/main.cpp` (\"The
   client\" below)."
4. Replace "`ladder_ledger_gui` mints a Local-mode session via `AppContext::login()` in Local mode, and dispatches
   `Login` against the server in Remote mode, mirroring bookmarks'/kanban's own `gui/main.cpp`." with "The `ledger`
   client installs its `--user` as the session locally and dispatches `Login` against a server
   (`app/controllers/boot_controller.cpp`)."
5. Replace "The QML views bind the pre-rendered `balanceText` / `limitText` / `spentText` / `amountText` the
   bridges publish." with "The views bind the balance, limit, spend and amount text the controllers render with it."
6. Replace "text the bridge pre-rendered through `ledger::formatMoney`" with "text the controllers render through
   `ledger::formatMoney`", and "No QML file divides anything." with "No view divides anything."
7. Replace "is a presenter-layer conversion — a dual-mode GUI test." with "is a client-layer conversion, pinned by
   `tests/client/test_statement_controller.cpp` (a late-January transfer in UTC-5)."

`codecov.yml`: replace `"examples/ledger/gui/**"` and `"examples/ledger/gui_wasm/**"` with `"examples/ledger/ui/**"`.

Comments that name the deleted client (each states what the code does now; replace exactly the quoted text):

- `include/ledger/core/money.hpp`: "a QML `numerator / denominator / Math.pow(10, places)` drifts" → "a floating-point
  `numerator / denominator / 10^places` drifts".
- `include/ledger/dto/transaction_dto.hpp`: "a WebSocket client and the QML bridge have no `DataMapper`, so the
  shipped Undo control" → "a WebSocket client has no `DataMapper`, so the accounts tab's Undo control".
- `include/ledger/models/ledger_model.hpp`: in the `Q_MOC_RUN` comment, "emitting
  `Lightweight::ledger::gui::LedgerPresenter` and failing to compile with \"no member named 'ledger' in namespace
  'Lightweight'\"." → "so a `Q_OBJECT` header that includes this one fails to compile its moc output (\"no member
  named 'ledger' in namespace 'Lightweight'\")."; delete the paragraph that begins "Latent until now rather than new";
  replace the `AccountRecord` comment's first paragraph ("Forward-declared rather than included: … failing to
  compile.") with "Forward-declared rather than included: `ledger_entity.hpp` pulls in
  `<Lightweight/DataMapper/DataMapper.hpp>`, which moc mis-parses (see above) in any `Q_OBJECT` header that reaches
  this one." and its last sentence ("This also matches rung 4's shape -- … never hit this.") with "Rung 4's model
  header has the same shape: core and dto headers only, never its own entity header."; in `ListTransactions`'
  comment, "The read that makes `UndoTransaction` -- and the Undo control `gui/qml/LedgerView.qml` ships -- drivable
  at all: before this, `JournalId` appeared in exactly one DTO field in the rung, and that field was
  `UndoTransaction`'s own input, so a client could only ever pass an id it had guessed." → "The read that makes
  `UndoTransaction` -- and the accounts tab's Undo control -- drivable: without it, `JournalId` appears in exactly
  one DTO field in the rung, `UndoTransaction`'s own input, so a client could only pass an id it had guessed."
- `src/server/main.cpp`: "The desktop client (`examples/ledger/gui/`) talks to this" → "The `ledger` client
  (`examples/ledger/ui/`, with `--server`) talks to this".
- `tests/test_ledger_list_transactions.cpp`: the paragraph "Before this action, … what a socket client and the QML
  bridge do not have." → "Without this action, `JournalId` appears in exactly one DTO field in the whole rung,
  `UndoTransaction`'s own *input*: `StoreTransaction` and `UndoTransaction` both answer with `GetLedgerResult`
  (accounts and balances), `GetLedger` the same, `ImportLedgerChunk` with counts. So the reversal this rung ships --
  and the accounts tab's Undo control -- could only be handed a number someone guessed; a test can reach around that
  through a `DataMapper`, which a socket client does not have."
- `tests/test_ledger_model_keys.cpp`: "the one the ladder's QML boundary has to be careful with elsewhere" → "the one
  any JavaScript-number boundary has to be careful with".
- `tests/test_ledger_units.cpp`: "where the double division the QML views used to do drifts" → "where a double
  division drifts".

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_ledger_tests ladder_ledger_server ledger
./build/all/examples/ledger/ladder_ledger_tests
```

Then the check from Step 1. Expected: every ledger case passes; the check prints "ledger's QML stack is gone".

Mutation check: `git checkout HEAD -- examples/ledger/tests/test_gui_qml_smoke.cpp` and rebuild. Expected: the build
fails (`MORPH_LADDER_QML_URI` is no longer defined for this rung). Delete it again.

- [ ] **Step 5: Commit**

```bash
git add -A examples/ledger codecov.yml
git commit -m "wip(ledger): retire ledger's QML, bridges, presenters and job poller; README client section

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task L11: Ledger group verification and squash

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the ladder suite**

```bash
cmake --build build/all && ctest --test-dir build/all -L ladder --output-on-failure
```

Expected: every test passes.

- [ ] **Step 2: No frontend**

```bash
cmake --build build/nofront --target ladder_ledger_tests ladder_ledger_app
./build/nofront/examples/ledger/ladder_ledger_tests "[ledger]"
```

Expected: builds; every case passes; there is no `ledger` target.

- [ ] **Step 3: Toolkit-free and UI-free checks**

```bash
git grep -n -E '#include <Q|morph/qt|morph/tui|morph/qt_quick' -- examples/ledger/app; test $? -eq 1 && echo ok-app
git grep -n 'morph/ui/' -- examples/ledger/app/controllers; test $? -eq 1 && echo ok-controllers
git grep -n -E '#include <Q' -- examples/ledger/include examples/ledger/src ':!examples/ledger/src/server'; \
  test $? -eq 1 && echo ok-domain
```

Expected: `ok-app`, `ok-controllers`, `ok-domain`.

- [ ] **Step 4: Sanitizers** — as Task K16 Step 4 with `-DMORPH_LADDER_RUNGS=ledger`, binary
  `build/clang-tsan/examples/ledger/ladder_ledger_tests`, filter `"[ledger][client],[ledger][stress]"`. Expected:
  clean.

- [ ] **Step 5: clang-tidy over the changed lines** — CONTRIBUTING's recipe, `origin/master...HEAD`, file count
  asserted non-zero. Expected: no findings in `examples/ledger/`.

- [ ] **Step 6: Commit any fixes** (`wip(ledger): fixes from the sanitizer and tidy gates`), or say there were none.

- [ ] **Step 7: Squash the group into its one commit**

Follow the master plan's "Squashing a part" procedure with key `ledger` and this message:

```text
examples/ledger: one app, any frontend

The ledger client becomes a toolkit-free app library (ladder_ledger_app):
a boot controller (the --user principal, locally or through Login, and an
optional seeded book), accounts, budgets, rules and statement controllers,
and four tab views of bindings. The statement job is a Query refreshed on
the frontend's scheduler that stops once the job settles; import op ids
come from the shared UUID helper. The report runner's QObject and QTimer
move into the server, so the domain library no longer links Qt. One binary
picks Qt Quick or the TUI at runtime.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure must show `examples/ledger: one app, any frontend` directly after
`examples/kanban: one app, any frontend`.

---
# Group `lims`

### Task M1: The lims app library — display strings

Lims' forms are runtime forms over the schemas its DTOs already emit (spec 4 §5); like kanban's, each is a
`FormSession` over `morph::forms::FormModel::forAction<A>()` whose submissions go through the handler that serves
them, with `morph::examples::mapCompletion` for the success hook and `morph::examples::Wiring` as every controller's
input — all shared (Parts 5 and 6). This task adds the rung's own display strings; `morph_add_rung` builds `app/`
into `ladder_lims_app` and links it into `ladder_lims_tests`.

**Files:**
- Create: `examples/lims/app/support/lims_format.hpp`, `examples/lims/app/support/lims_format.cpp`
- Test: `examples/lims/tests/client/test_lims_format.cpp`

**Interfaces:**
- Consumes: `morph::units::toDecimalString` (`morph/util/quantity.hpp`), `lims::Concentration`
  (`lims/core/types.hpp:231`), `stateName`, `qualifierName`, `conflictReasonName`, `conflictStatusName` (the DTO
  headers).
- Produces: `lims::client::ResultRow{id, idText, reading, outOfSpec, capturedBy}`, `ConflictRow{id, text}`, `AnalysisRow{versionId,
  name}`; `valueText(Concentration const&)`, `unitText()`, `sampleHeader(std::optional<SampleView> const&)`,
  `resultRow(ResultView const&)`, `conflictRow(ConflictView const&)`, `analysisRow(AnalysisVersionView const&)`.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/client/test_lims_format.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/util/rational.hpp>
#include <optional>

#include "support/lims_format.hpp"

namespace {

[[nodiscard]] morph::math::Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return morph::math::Rational{morph::math::Numerator{num}, morph::math::Denominator{den},
                                 morph::math::DecimalPlaces{places}};
}

}  // namespace

TEST_CASE("lims::client: quantity text is exact and empty-safe", "[lims][client]") {
    CHECK(lims::client::valueText(lims::Concentration{exact(1, 400, 4)}) == "0.0025");
    CHECK(lims::client::valueText(lims::Concentration{}).empty());
    CHECK(lims::client::unitText() == "mg/L");
}

TEST_CASE("lims::client: a result row names its reading, or its qualifier when it has no number", "[lims][client]") {
    lims::ResultView measured{.id = lims::ResultId{3},
                              .sampleId = lims::SampleId{1},
                              .analysisVersionId = lims::AnalysisVersionId{2},
                              .qualifier = lims::ResultQualifier::Measured,
                              .value = lims::Concentration{exact(12, 5, 3)},
                              .outOfSpec = true,
                              .capturedBy = "alice",
                              .capturedAt = {}};
    auto const row = lims::client::resultRow(measured);
    CHECK(row.idText == "3");
    CHECK(row.reading == "2.4 mg/L");
    CHECK(row.outOfSpec == "out of spec");
    CHECK(row.capturedBy == "alice");

    lims::ResultView qualified = measured;
    qualified.value = lims::Concentration{};
    qualified.qualifier = lims::ResultQualifier::BelowLOD;
    qualified.outOfSpec = false;
    auto const noNumber = lims::client::resultRow(qualified);
    CHECK(noNumber.reading == "belowLOD");
    CHECK(noNumber.outOfSpec.empty());
}

TEST_CASE("lims::client::sampleHeader names the sample, its state and version", "[lims][client]") {
    CHECK(lims::client::sampleHeader(std::nullopt) == "no sample attached");
    lims::SampleView const sample{.id = lims::SampleId{7},
                                  .clientId = lims::ClientId{1},
                                  .reference = "WW-1",
                                  .state = lims::SampleState::InProgress,
                                  .version = lims::SampleVersion{3},
                                  .registeredAt = {}};
    CHECK(lims::client::sampleHeader(sample) == "sample 7 — in-progress (v3)");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_lims_tests`
Expected: compile error, `'support/lims_format.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/lims/app/support/lims_format.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "lims/core/types.hpp"
#include "lims/dto/analysis_dto.hpp"
#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"

namespace lims::client {

/// @brief One captured result.
struct ResultRow {
    /// @brief The result's id.
    std::int64_t id = 0;
    /// @brief That id as text.
    std::string idText;
    /// @brief The reading with its unit, or the qualifier when there is no number.
    std::string reading;
    /// @brief `"out of spec"`, or empty.
    std::string outOfSpec;
    /// @brief Who captured it.
    std::string capturedBy;
    /// @brief Compared by value.
    bool operator==(ResultRow const&) const = default;
};

/// @brief One replay conflict.
struct ConflictRow {
    /// @brief The conflict's id.
    std::int64_t id = 0;
    /// @brief Its display line.
    std::string text;
    /// @brief Compared by value.
    bool operator==(ConflictRow const&) const = default;
};

/// @brief One analysis version in the catalogue picker.
struct AnalysisRow {
    /// @brief The analysis version's id.
    std::int64_t versionId = 0;
    /// @brief The analysis' name.
    std::string name;
    /// @brief Compared by value.
    bool operator==(AnalysisRow const&) const = default;
};

/// @brief A concentration as exact decimal text. @param quantity The value. @return The digits, or empty.
[[nodiscard]] std::string valueText(Concentration const& quantity);
/// @brief The canonical concentration unit's display text. @return `"mg/L"`.
[[nodiscard]] std::string unitText();
/// @brief The shell's header line.
/// @param sample The attached sample, if any.
/// @return `"sample <id> — <state> (v<version>)"`, or `"no sample attached"`.
[[nodiscard]] std::string sampleHeader(std::optional<SampleView> const& sample);
/// @brief A result as a list row. @param view The result. @return The row.
[[nodiscard]] ResultRow resultRow(ResultView const& view);
/// @brief A conflict as a list row. @param view The conflict. @return The row.
[[nodiscard]] ConflictRow conflictRow(ConflictView const& view);
/// @brief An analysis version as a picker row. @param view The version. @return The row.
[[nodiscard]] AnalysisRow analysisRow(AnalysisVersionView const& view);

}  // namespace lims::client
```

Create `examples/lims/app/support/lims_format.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "support/lims_format.hpp"

#include <morph/util/quantity.hpp>
#include <string>

namespace lims::client {

std::string valueText(Concentration const& quantity) {
    if (!quantity.hasValue()) {
        return {};
    }
    return morph::units::toDecimalString(quantity);
}

std::string unitText() { return std::string{Concentration::unitMeta().display}; }

std::string sampleHeader(std::optional<SampleView> const& sample) {
    if (!sample || !sample->id.hasValue()) {
        return "no sample attached";
    }
    return "sample " + std::to_string(*sample->id) + " — " + std::string{stateName(sample->state)} + " (v" +
           std::to_string(*sample->version) + ")";
}

ResultRow resultRow(ResultView const& view) {
    std::int64_t const resultId = view.id.hasValue() ? *view.id : 0;
    return ResultRow{.id = resultId,
                     .idText = std::to_string(resultId),
                     .reading = view.value.hasValue() ? valueText(view.value) + " " + unitText()
                                                      : std::string{qualifierName(view.qualifier)},
                     .outOfSpec = view.outOfSpec ? std::string{"out of spec"} : std::string{},
                     .capturedBy = view.capturedBy};
}

ConflictRow conflictRow(ConflictView const& view) {
    std::int64_t const conflictId = view.id.hasValue() ? *view.id : 0;
    return ConflictRow{.id = conflictId,
                       .text = "conflict " + std::to_string(conflictId) + " — " +
                               std::string{conflictReasonName(view.reason)} + " (base v" +
                               std::to_string(*view.baseVersion) + " vs server v" +
                               std::to_string(*view.serverVersion) + ") — " +
                               std::string{conflictStatusName(view.status)}};
}

AnalysisRow analysisRow(AnalysisVersionView const& view) {
    return AnalysisRow{.versionId = view.id.hasValue() ? *view.id : 0, .name = view.name};
}

}  // namespace lims::client
```

Re-run the configure (`cmake build/all`) so `morph_add_rung`'s globs see the new `app/` and `tests/client/` files.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_lims_tests
./build/all/examples/lims/ladder_lims_tests "[lims][client]"
```

Expected: PASS, 3 cases.

Mutation check: in `resultRow`, drop the `+ " " + unitText()`. Expected FAIL in "a result row names its reading…".
Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/lims/app examples/lims/tests/client
git commit -m "wip(lims): app library foundations: display strings

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task M2: `SampleController` — the lifecycle tab

Re-expresses `SamplePresenter`/`SampleBridge` and `SampleView.qml`: the registration, reject and rework forms are
runtime forms (RegisterClient through a plain handler — it carries no key, and an `AllowShared` handler is not bound
until it attaches; the others through the shared one); the attached sample is a `Query<OpenSample>` keyed on the
sample asked for (a registration's reply, or "Open sample"); the four zero-field transitions are `Mutation`s that
invalidate it; and which button is enabled in which state is a controller projection, not a QML expression.

**Files:**
- Create: `examples/lims/app/controllers/sample_controller.hpp`, `examples/lims/app/controllers/sample_controller.cpp`
- Create: `examples/lims/tests/client/lims_client_support.hpp`
- Test: `examples/lims/tests/client/test_sample_controller.cpp`

**Interfaces:**
- Consumes: Task M1; `lims::SampleModel`, `RegisterClient`/`RegisterClientResult`, `RegisterSample`, `OpenSample`,
  `ReceiveSample`, `StartWork`, `SubmitForVerification`, `PublishSample`, `RejectSample`, `ReturnForRework`,
  `SampleView`, `stateName`.
- Produces: `SampleController(examples::Wiring)`: `registerClientForm()`, `registerSampleForm()`, `rejectForm()`, `reworkForm()`,
  `sample()`, `sampleId()`, `headerText()`, `state()`, `canReceive()`, `canStartWork()`, `canSubmit()`,
  `canPublish()`, `canRework()`, `canReject()`, `receive()`, `startWork()`, `submitForVerification()`, `publish()`,
  `openSample(SampleId)`, `refresh()`, `openIdText()`, `setOpenIdText(std::string)`, `canOpen()`, `openTyped()`,
  `clientText()`, `errorText()`, `busy()`, `registrar()`. Test support (`lims::testing`): `LocalClient`, `fill`,
  `submitForm`, `submitFormExpectingRefusal`, `registerSample(client, samples)`.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/client/lims_client_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "app/wiring.hpp"
#include "controllers/sample_controller.hpp"
#include "testkit/wait.hpp"

namespace lims::testing {

using namespace std::chrono_literals;

/// A client over a local backend on a MainThreadExecutor, signed in as @p principal.
class LocalClient {
public:
    explicit LocalClient(std::string principal) {
        morph::session::Context session;
        session.principal = std::move(principal);
        bridge.setDefaultSession(std::move(session));
    }
    ~LocalClient() = default;
    LocalClient(LocalClient const&) = delete;
    LocalClient& operator=(LocalClient const&) = delete;
    LocalClient(LocalClient&&) = delete;
    LocalClient& operator=(LocalClient&&) = delete;

    template <typename Pred>
    [[nodiscard]] bool settle(Pred done, std::chrono::milliseconds budget = 5000ms) {
        return morph::examples::testing::pumpUntil(owner, std::move(done), budget);
    }
    [[nodiscard]] morph::examples::Wiring wiring() {
        return morph::examples::Wiring{
            .runtime = runtime, .scheduler = scheduler, .bridge = bridge, .callbacks = owner};
    }

    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::reactive::Runtime runtime{owner};
    morph::reactive::testing::ManualScheduler scheduler;
};

/// Sets each member @p bodyJson names, as typing into those fields would, and leaves every other field alone — the
/// context its controller filled in (the registered client) stays. `prefill` would blank that context.
inline void fill(morph::forms::FormSession& form, std::string_view bodyJson) {
    glz::generic_u64 members;
    REQUIRE_FALSE(glz::read_json(members, std::string{bodyJson}));
    REQUIRE(members.is_object());
    for (auto const& [name, value] : members.get_object()) {
        form.assign(name, glz::write_json(value).value_or(std::string{"null"}));
    }
}

/// Fills @p form, submits it, waits for the outcome and requires success.
inline void submitForm(LocalClient& client, morph::forms::FormSession& form, std::string_view bodyJson) {
    fill(form, bodyJson);
    REQUIRE(form.ready());
    form.submit();
    REQUIRE(client.settle([&form] { return !form.pending(); }));
    INFO("the form reported: " << morph::reactive::errorMessage(form.lastError()));
    REQUIRE(form.lastError() == nullptr);
}

/// Fills @p form, submits it, requires a refusal and returns its message.
[[nodiscard]] inline std::string submitFormExpectingRefusal(LocalClient& client, morph::forms::FormSession& form,
                                                            std::string_view bodyJson) {
    fill(form, bodyJson);
    REQUIRE(form.ready());
    form.submit();
    REQUIRE(client.settle([&form] { return !form.pending(); }));
    REQUIRE(form.lastError() != nullptr);
    return morph::reactive::errorMessage(form.lastError());
}

/// Registers a client and a sample "WW-1" through @p samples' own forms; the sample ends up attached.
inline void registerSample(LocalClient& client, lims::client::SampleController& samples) {
    submitForm(client, samples.registerClientForm(), R"({"name":"Waterworks Ltd"})");
    REQUIRE(client.settle([&samples] { return samples.clientText() != "Latest client id: -1"; }));
    submitForm(client, samples.registerSampleForm(), R"({"reference":"WW-1"})");
    REQUIRE(client.settle([&samples] { return samples.sample().has_value(); }));
}

}  // namespace lims::testing
```

Create `examples/lims/tests/client/test_sample_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "controllers/sample_controller.hpp"
#include "lims_client_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::SampleState;
using lims::client::SampleController;
using lims::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

TEST_CASE("lims::client::SampleController routes registration and every lifecycle transition", "[lims][client]") {
    DbFixture const fixture;
    LocalClient client{"alice"};
    SampleController samples{client.wiring()};
    CHECK(samples.headerText() == "no sample attached");
    CHECK(samples.clientText() == "Latest client id: -1");

    lims::testing::registerSample(client, samples);

    CHECK(samples.sample()->state == SampleState::Registered);
    CHECK(samples.headerText().starts_with("sample "));
    CHECK(samples.canReceive());
    CHECK(samples.canReject());
    CHECK_FALSE(samples.canPublish());

    samples.receive();
    REQUIRE(client.settle([&samples] { return samples.state() == "received"; }));
    CHECK(samples.canStartWork());
    samples.startWork();
    REQUIRE(client.settle([&samples] { return samples.state() == "in-progress"; }));
    CHECK(samples.canSubmit());
    samples.submitForVerification();
    REQUIRE(client.settle([&samples] { return samples.state() == "to-be-verified"; }));
    CHECK(samples.canPublish());
    CHECK(samples.canRework());
    CHECK_FALSE(samples.canReject());
    samples.publish();
    REQUIRE(client.settle([&samples] { return samples.state() == "published"; }));
    CHECK_FALSE(samples.busy());
}

TEST_CASE("lims::client::SampleController surfaces a model refusal as a displayable message", "[lims][client]") {
    DbFixture const fixture;
    LocalClient client{"alice"};
    SampleController samples{client.wiring()};
    lims::testing::registerSample(client, samples);

    samples.publish();

    REQUIRE(client.settle([&samples] { return !samples.errorText().empty(); }));
    CHECK(samples.errorText().find("registered") != std::string::npos);
    CHECK(samples.errorText().find("published") != std::string::npos);
    CHECK_FALSE(samples.busy());
}

TEST_CASE("lims::client::SampleController reports busy while a transition is in flight", "[lims][client]") {
    DbFixture const fixture;
    LocalClient client{"alice"};
    SampleController samples{client.wiring()};
    lims::testing::registerSample(client, samples);
    CHECK_FALSE(samples.busy());

    samples.receive();

    CHECK(samples.busy());
    REQUIRE(client.settle([&samples] { return !samples.busy(); }));
}

TEST_CASE("lims::client::SampleController opens an existing sample by id and refuses a bad one",
          "[lims][client]") {
    DbFixture const fixture;
    LocalClient client{"alice"};
    SampleController first{client.wiring()};
    lims::testing::registerSample(client, first);
    std::string const sampleId = std::to_string(*first.sample()->id);
    SampleController second{client.wiring()};

    second.setOpenIdText("abc");
    CHECK_FALSE(second.canOpen());
    second.setOpenIdText(sampleId);
    REQUIRE(second.canOpen());
    second.openTyped();

    REQUIRE(client.settle([&second] { return second.sample().has_value(); }));
    CHECK(std::to_string(*second.sample()->id) == sampleId);
}

TEST_CASE("lims::client::SampleController: the reject form's refusal is the model's own message", "[lims][client]") {
    DbFixture const fixture;
    LocalClient client{"alice"};
    SampleController samples{client.wiring()};
    lims::testing::registerSample(client, samples);
    samples.receive();
    REQUIRE(client.settle([&samples] { return samples.state() == "received"; }));
    samples.startWork();
    REQUIRE(client.settle([&samples] { return samples.state() == "in-progress"; }));

    std::string const message =
        lims::testing::submitFormExpectingRefusal(client, samples.rejectForm(), R"({"reason":"seal broken"})");

    CHECK_FALSE(message.empty());
    CHECK(samples.state() == "in-progress");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_lims_tests`
Expected: compile error, `'controllers/sample_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/lims/app/controllers/sample_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "app/wiring.hpp"
#include "lims/models/sample_model.hpp"

namespace lims::client {

/// @brief The lifecycle tab: register a client and a sample, open one, move it through its lifecycle, return it
///        for rework or reject it.
class SampleController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    explicit SampleController(morph::examples::Wiring wiring);

    /// @brief The `RegisterClient` form. @return It.
    [[nodiscard]] morph::forms::FormSession& registerClientForm() noexcept { return _registerClient; }
    /// @brief The `RegisterSample` form; the last registered client is filled in. @return It.
    [[nodiscard]] morph::forms::FormSession& registerSampleForm() noexcept { return _registerSample; }
    /// @brief The `RejectSample` form. @return It.
    [[nodiscard]] morph::forms::FormSession& rejectForm() noexcept { return _reject; }
    /// @brief The `ReturnForRework` form. @return It.
    [[nodiscard]] morph::forms::FormSession& reworkForm() noexcept { return _rework; }
    /// @brief The attached sample. Tracked. @return Its state, kept while it refreshes; empty before one is attached.
    [[nodiscard]] std::optional<SampleView> const& sample() const { return _open.value(); }
    /// @brief The attached sample's id. Tracked, equality-gated. @return The id, or empty.
    [[nodiscard]] std::optional<SampleId> sampleId() const { return _sampleId.get(); }
    /// @brief Tracked. @return The shell's header line.
    [[nodiscard]] std::string headerText() const;
    /// @brief Tracked. @return The attached sample's state name, or empty.
    [[nodiscard]] std::string state() const;
    /// @brief Tracked. @return Whether "Receive" applies (registered).
    [[nodiscard]] bool canReceive() const;
    /// @brief Tracked. @return Whether "Start work" applies (received).
    [[nodiscard]] bool canStartWork() const;
    /// @brief Tracked. @return Whether "Submit for verification" applies (in progress).
    [[nodiscard]] bool canSubmit() const;
    /// @brief Tracked. @return Whether "Publish" applies (to be verified).
    [[nodiscard]] bool canPublish() const;
    /// @brief Tracked. @return Whether the rework form applies (to be verified).
    [[nodiscard]] bool canRework() const;
    /// @brief Tracked. @return Whether the reject form applies (registered or received).
    [[nodiscard]] bool canReject() const;
    /// @brief Receives the attached sample.
    void receive();
    /// @brief Starts work on it.
    void startWork();
    /// @brief Submits it for verification.
    void submitForVerification();
    /// @brief Publishes it.
    void publish();
    /// @brief Attaches to an existing sample. @param sample The sample.
    void openSample(SampleId sample);
    /// @brief Fetches the attached sample again.
    void refresh();
    /// @brief Tracked. @return The "Open existing" id input.
    [[nodiscard]] std::string const& openIdText() const { return _openIdText.get(); }
    /// @brief Sets the "Open existing" id input. @param text The id as typed.
    void setOpenIdText(std::string text) { _openIdText.set(std::move(text)); }
    /// @brief Tracked. @return Whether the id input names a positive id.
    [[nodiscard]] bool canOpen() const;
    /// @brief Attaches to the sample the id input names.
    void openTyped();
    /// @brief Tracked. @return `"Latest client id: <id>"` (`-1` before one is registered).
    [[nodiscard]] std::string clientText() const;
    /// @brief Tracked. @return The first failure of the attach or a transition, or empty.
    [[nodiscard]] std::string errorText() const;
    /// @brief Tracked. @return Whether the attach or a transition is in flight.
    [[nodiscard]] bool busy() const;
    /// @brief The plain `SampleModel` handler, bound from construction: it serves the actions that name no sample —
    ///        `RegisterClient`, and the capture form's qualifier and dilution lists.
    /// @return The handler; it lives as long as this controller.
    [[nodiscard]] morph::bridge::BridgeHandler<SampleModel>& registrar() noexcept { return _creator; }

private:
    [[nodiscard]] morph::async::Completion<std::string> submit(std::string_view actionType, std::string body);
    void onReply(std::string_view actionType, std::string const& reply);
    [[nodiscard]] bool stateIs(SampleState wanted) const;

    morph::examples::Wiring _wiring;
    morph::bridge::BridgeHandler<SampleModel> _creator;
    morph::bridge::BridgeHandler<SampleModel, morph::bridge::AllowShared> _handler;
    morph::reactive::Signal<std::optional<SampleId>> _requested;
    morph::reactive::Signal<std::optional<ClientId>> _client;
    morph::reactive::Signal<std::string> _openIdText;
    morph::reactive::Query<OpenSample> _open;
    morph::reactive::Computed<std::optional<SampleId>> _sampleId;
    morph::reactive::Mutation<ReceiveSample> _receive;
    morph::reactive::Mutation<StartWork> _startWork;
    morph::reactive::Mutation<SubmitForVerification> _submitForVerification;
    morph::reactive::Mutation<PublishSample> _publish;
    morph::forms::FormSession _registerClient;
    morph::forms::FormSession _registerSample;
    morph::forms::FormSession _reject;
    morph::forms::FormSession _rework;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace lims::client
```

Create `examples/lims/app/controllers/sample_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/sample_controller.hpp"

#include <charconv>
#include <exception>
#include <glaze/glaze.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <system_error>
#include <utility>

#include "app/completion_map.hpp"
#include "support/lims_format.hpp"

namespace lims::client {

namespace {

[[nodiscard]] std::optional<std::int64_t> positiveId(std::string_view text) {
    std::int64_t value = 0;
    auto const [end, failure] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || failure != std::errc{} || end != text.data() + text.size() || value <= 0) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

SampleController::SampleController(morph::examples::Wiring wiring)
    : _wiring{wiring},
      _creator{wiring.bridge, &wiring.callbacks},
      _handler{wiring.bridge, &wiring.callbacks},
      _requested{wiring.runtime, std::nullopt},
      _client{wiring.runtime, std::nullopt},
      _openIdText{wiring.runtime, std::string{}},
      _open{wiring.runtime, _handler,
            [this]() -> std::optional<OpenSample> {
                auto const& requested = _requested.get();
                if (!requested) {
                    return std::nullopt;
                }
                return OpenSample{.sampleId = *requested};
            }},
      _sampleId{wiring.runtime,
                [this]() -> std::optional<SampleId> {
                    auto const& view = _open.value();
                    if (!view) {
                        return std::nullopt;
                    }
                    return view->id;
                }},
      _receive{wiring.runtime, _handler, morph::reactive::MutationOptions{.invalidates = {&_open}}},
      _startWork{wiring.runtime, _handler, morph::reactive::MutationOptions{.invalidates = {&_open}}},
      _submitForVerification{wiring.runtime, _handler, morph::reactive::MutationOptions{.invalidates = {&_open}}},
      _publish{wiring.runtime, _handler, morph::reactive::MutationOptions{.invalidates = {&_open}}},
      _registerClient{wiring.runtime, morph::forms::FormModel::forAction<RegisterClient>(),
                      [this](std::string_view actionType, std::string body) { return submit(actionType, std::move(body)); },
                      {}},
      _registerSample{wiring.runtime, morph::forms::FormModel::forAction<RegisterSample>(),
                      [this](std::string_view actionType, std::string body) { return submit(actionType, std::move(body)); },
                      {}},
      _reject{wiring.runtime, morph::forms::FormModel::forAction<RejectSample>(),
              [this](std::string_view actionType, std::string body) { return submit(actionType, std::move(body)); },
              {}},
      _rework{wiring.runtime, morph::forms::FormModel::forAction<ReturnForRework>(),
              [this](std::string_view actionType, std::string body) { return submit(actionType, std::move(body)); },
              {}} {}

std::string SampleController::headerText() const { return sampleHeader(_open.value()); }

std::string SampleController::state() const {
    auto const& view = _open.value();
    return view ? std::string{stateName(view->state)} : std::string{};
}

bool SampleController::stateIs(SampleState wanted) const {
    auto const& view = _open.value();
    return view.has_value() && view->state == wanted;
}

bool SampleController::canReceive() const { return stateIs(SampleState::Registered); }
bool SampleController::canStartWork() const { return stateIs(SampleState::Received); }
bool SampleController::canSubmit() const { return stateIs(SampleState::InProgress); }
bool SampleController::canPublish() const { return stateIs(SampleState::ToBeVerified); }
bool SampleController::canRework() const { return stateIs(SampleState::ToBeVerified); }
bool SampleController::canReject() const {
    return stateIs(SampleState::Registered) || stateIs(SampleState::Received);
}

void SampleController::receive() { _receive.run(ReceiveSample{}); }
void SampleController::startWork() { _startWork.run(StartWork{}); }
void SampleController::submitForVerification() { _submitForVerification.run(SubmitForVerification{}); }
void SampleController::publish() { _publish.run(PublishSample{}); }

void SampleController::openSample(SampleId sample) {
    if (_requested.peek() == std::optional<SampleId>{sample}) {
        _open.refetch();
        return;
    }
    _requested.set(sample);
}

void SampleController::refresh() { _open.refetch(); }

bool SampleController::canOpen() const { return positiveId(_openIdText.get()).has_value(); }

void SampleController::openTyped() {
    if (auto const sampleId = positiveId(_openIdText.peek())) {
        openSample(SampleId{*sampleId});
    }
}

std::string SampleController::clientText() const {
    auto const& client = _client.get();
    return "Latest client id: " + std::to_string(client && client->hasValue() ? **client : -1);
}

std::string SampleController::errorText() const {
    for (std::exception_ptr const& error : {_open.error(), _receive.error(), _startWork.error(),
                                            _submitForVerification.error(), _publish.error()}) {
        if (error != nullptr) {
            return morph::reactive::errorMessage(error);
        }
    }
    return {};
}

bool SampleController::busy() const {
    return _open.pending() || _receive.pending() || _startWork.pending() || _submitForVerification.pending() ||
           _publish.pending();
}

morph::async::Completion<std::string> SampleController::submit(std::string_view actionType, std::string body) {
    // RegisterClient names no sample, so it cannot run on the shared handler before that handler has attached;
    // every other form acts on the attached sample (RegisterSample attaches it from its own reply).
    auto completion = actionType == morph::model::ActionTraits<RegisterClient>::typeId()
                          ? _creator.executeJson(actionType, body)
                          : _handler.executeJson(actionType, body);
    return morph::examples::mapCompletion<std::string>(
        _wiring.callbacks, _lifetime.token(), std::move(completion),
        [this, type = std::string{actionType}](std::string const& reply) {
            onReply(type, reply);
            return reply;
        },
        [](std::exception_ptr const&) {});
}

void SampleController::onReply(std::string_view actionType, std::string const& reply) {
    if (actionType == morph::model::ActionTraits<RegisterClient>::typeId()) {
        RegisterClientResult result;
        if (!glz::read_json(result, reply) && result.clientId.hasValue()) {
            _client.set(result.clientId);
            _registerClient.reset();
            _registerSample.reset();
            _registerSample.assign("clientId", std::to_string(*result.clientId));
        }
        return;
    }
    if (actionType == morph::model::ActionTraits<RegisterSample>::typeId()) {
        SampleView view;
        if (!glz::read_json(view, reply) && view.id.hasValue()) {
            _registerSample.reset();
            if (auto const& client = _client.peek(); client && client->hasValue()) {
                _registerSample.assign("clientId", std::to_string(**client));
            }
            openSample(view.id);
        }
        return;
    }
    if (actionType == morph::model::ActionTraits<RejectSample>::typeId()) {
        _reject.reset();
    } else {
        _rework.reset();
    }
    _open.refetch();
}

}  // namespace lims::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_lims_tests
./build/all/examples/lims/ladder_lims_tests "[lims][client]"
```

Expected: PASS.

Mutation check: in `submit`, route every action to `_handler`. Expected FAIL in every case that registers (the
`RegisterClient` submission is refused: "handler not bound"). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/lims/app/controllers/sample_controller.hpp examples/lims/app/controllers/sample_controller.cpp \
        examples/lims/tests/client/lims_client_support.hpp examples/lims/tests/client/test_sample_controller.cpp
git commit -m "wip(lims): SampleController: registration, attach and the sample lifecycle

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task M3: `ResultController` — the results tab

Re-expresses `ResultPresenter`/`ResultBridge` and `ResultEntryView.qml`: the catalogue is a `Query<ListAnalyses>`;
the results tab's own `AllowShared` handler attaches to whichever sample the lifecycle tab has open (a
`Query<OpenSample>` keyed on `SampleController::sampleId()` — `Main.qml`'s `onSampleChanged` hand-off, as a key);
results and conflicts are queries keyed on that attachment; verify is a `Mutation` invalidating the results; the
capture and resolve forms are runtime forms whose success refreshes results and conflicts. The capture form's two
Choice fields fetch their lists (`ListResultQualifiers`, `ListDilutionModes`) through `forms::handlerChoiceFetcher`
over the lifecycle tab's plain `SampleModel` handler (`SampleController::registrar()`): the lists name no sample, the
form fetches them as it is constructed, and this tab's own `AllowShared` handler is not bound until it attaches.

**Files:**
- Create: `examples/lims/app/controllers/result_controller.hpp`, `examples/lims/app/controllers/result_controller.cpp`
- Test: `examples/lims/tests/client/test_result_controller.cpp`

**Interfaces:**
- Consumes: `forms::handlerChoiceFetcher` (Part 5, `<morph/forms/engine/handler_submitter.hpp>`);
  `SampleController::sampleId()`, `registrar()`, `receive()`, `startWork()`, `state()` (Task M2); `resultRow`,
  `conflictRow`, `analysisRow` (Task M1); `morph::examples::mapCompletion` (Part 6);
  `forms::FormModel::forAction<A>()` (Part 5); `lims::AnalysisCatalogModel`,
  `ListAnalyses`, `DefineAnalysis`, `SampleModel`, `OpenSample`, `ListResults`, `ListConflicts`, `VerifyResult`,
  `VerificationView`, `CaptureConcentration`, `ResolveConflict`, `QualifierChoice`, `kQualifierBelowLod`.
- Produces: `ResultController(examples::Wiring, SampleController&)`: `analyses()`, `refreshAnalyses()`, `picked()`,
  `pick(std::int64_t)`, `pickedText()`, `captureForm()`, `resolveForm()`, `results()`, `refreshResults()`,
  `conflicts()`, `refreshConflicts()`, `hasConflicts()`, `verify(std::int64_t resultId)`, `attached()`,
  `statusText()`, `errorText()`.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/client/test_result_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <utility>

#include "controllers/result_controller.hpp"
#include "controllers/sample_controller.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims_client_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::client::ResultController;
using lims::client::SampleController;
using lims::testing::LocalClient;
using morph::ladder::testkit::DbFixture;

namespace {

[[nodiscard]] morph::math::Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return morph::math::Rational{morph::math::Numerator{num}, morph::math::Denominator{den},
                                 morph::math::DecimalPlaces{places}};
}

// Defines an analysis straight through the catalogue model, as "alice".
[[nodiscard]] lims::AnalysisVersionId defineAnalysis(std::string name) {
    morph::session::Context context;
    context.principal = "alice";
    morph::session::detail::ScopedContext const scope{context};
    lims::AnalysisCatalogModel catalog;
    return catalog.execute(lims::DefineAnalysis{.name = std::move(name), .canonicalUnit = "mg_per_L", .decimalPlaces = 3})
        .versionId;
}

// Registers a sample and moves it to in-progress, where results can be captured.
void sampleInProgress(LocalClient& client, SampleController& samples) {
    lims::testing::registerSample(client, samples);
    samples.receive();
    REQUIRE(client.settle([&samples] { return samples.state() == "received"; }));
    samples.startWork();
    REQUIRE(client.settle([&samples] { return samples.state() == "in-progress"; }));
}

[[nodiscard]] std::string captureBody(lims::CaptureConcentration const& capture) {
    auto const body = glz::write_json(capture);
    REQUIRE(body.has_value());
    return *body;
}

}  // namespace

TEST_CASE("lims::client::ResultController follows the lifecycle tab's sample", "[lims][client]") {
    DbFixture const fixture;
    LocalClient client{"alice"};
    SampleController samples{client.wiring()};
    ResultController results{client.wiring(), samples};
    CHECK_FALSE(results.attached());

    lims::testing::registerSample(client, samples);

    REQUIRE(client.settle([&results] { return results.attached(); }));
    CHECK(results.results().empty());
    CHECK_FALSE(results.hasConflicts());
    CHECK(results.analyses().empty());
    CHECK(results.errorText().empty());
}

TEST_CASE("lims::client::ResultController: a captured result carries the exact decimal", "[lims][client]") {
    DbFixture const fixture;
    auto const nitrate = defineAnalysis("Nitrate");
    LocalClient client{"alice"};
    SampleController samples{client.wiring()};
    ResultController results{client.wiring(), samples};
    sampleInProgress(client, samples);
    REQUIRE(client.settle([&results] { return results.attached(); }));

    results.refreshAnalyses();
    REQUIRE(client.settle([&results] { return results.analyses().size() == 1; }));
    results.pick(results.analyses().front().versionId);
    CHECK(results.pickedText() == "version id " + std::to_string(*nitrate));

    lims::testing::submitForm(client, results.captureForm(),
                              captureBody(lims::CaptureConcentration{.analysisVersionId = nitrate,
                                                                     .value = lims::Concentration{exact(12, 5, 3)}}));

    REQUIRE(client.settle([&results] { return results.results().size() == 1; }));
    auto const row = results.results().front();
    CHECK(row.reading == "2.4 mg/L");
    CHECK(row.capturedBy == "alice");
    CHECK(row.outOfSpec.empty());
}

TEST_CASE("lims::client::ResultController: a no-number result names its qualifier", "[lims][client]") {
    DbFixture const fixture;
    auto const lead = defineAnalysis("Lead");
    LocalClient client{"alice"};
    SampleController samples{client.wiring()};
    ResultController results{client.wiring(), samples};
    sampleInProgress(client, samples);
    REQUIRE(client.settle([&results] { return results.attached(); }));

    lims::testing::submitForm(
        client, results.captureForm(),
        captureBody(lims::CaptureConcentration{.analysisVersionId = lead,
                                               .qualifier = lims::QualifierChoice{std::string{lims::kQualifierBelowLod}}}));

    REQUIRE(client.settle([&results] { return results.results().size() == 1; }));
    CHECK(results.results().front().reading == "belowLOD");
}

TEST_CASE("lims::client::ResultController: verifying one's own result is refused, and says so", "[lims][client]") {
    DbFixture const fixture;
    auto const nitrate = defineAnalysis("Nitrate");
    LocalClient client{"alice"};
    SampleController samples{client.wiring()};
    ResultController results{client.wiring(), samples};
    sampleInProgress(client, samples);
    REQUIRE(client.settle([&results] { return results.attached(); }));
    lims::testing::submitForm(client, results.captureForm(),
                              captureBody(lims::CaptureConcentration{.analysisVersionId = nitrate,
                                                                     .value = lims::Concentration{exact(12, 5, 3)}}));
    REQUIRE(client.settle([&results] { return results.results().size() == 1; }));

    results.verify(results.results().front().id);

    REQUIRE(client.settle([&results] { return !results.errorText().empty(); }));
    CHECK(results.statusText().empty());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_lims_tests`
Expected: compile error, `'controllers/result_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/lims/app/controllers/result_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/handler_submitter.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/wiring.hpp"
#include "controllers/sample_controller.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/sample_model.hpp"
#include "support/lims_format.hpp"

namespace lims::client {

/// @brief The results tab: the analysis catalogue, capture, the attached sample's results with verify, and replay
///        conflicts with their resolution.
class ResultController {
public:
    /// @param wiring The runtime, bridge and owner this controller uses.
    /// @param samples The lifecycle tab, whose open sample this tab attaches to and whose plain handler serves the
    ///        capture form's lists. Borrowed: it must outlive this.
    ResultController(morph::examples::Wiring wiring, SampleController& samples);

    /// @brief Tracked. @return The catalogue's analysis versions.
    [[nodiscard]] std::vector<AnalysisRow> analyses() const;
    /// @brief Fetches the catalogue again.
    void refreshAnalyses() { _analyses.refetch(); }
    /// @brief Tracked. @return The picked analysis version, or empty.
    [[nodiscard]] std::optional<std::int64_t> const& picked() const { return _picked.get(); }
    /// @brief Picks an analysis version. @param versionId The version.
    void pick(std::int64_t versionId) { _picked.set(versionId); }
    /// @brief Tracked. @return `"version id <n>"` for the picked version, or empty.
    [[nodiscard]] std::string pickedText() const;
    /// @brief The `CaptureConcentration` form. @return It.
    [[nodiscard]] morph::forms::FormSession& captureForm() noexcept { return _capture; }
    /// @brief The `ResolveConflict` form. @return It.
    [[nodiscard]] morph::forms::FormSession& resolveForm() noexcept { return _resolve; }
    /// @brief Tracked. @return The attached sample's results.
    [[nodiscard]] std::vector<ResultRow> results() const;
    /// @brief Fetches the results again.
    void refreshResults() { _results.refetch(); }
    /// @brief Tracked. @return The attached sample's replay conflicts.
    [[nodiscard]] std::vector<ConflictRow> conflicts() const;
    /// @brief Fetches the conflicts again.
    void refreshConflicts() { _conflicts.refetch(); }
    /// @brief Tracked. @return Whether there is a conflict to resolve.
    [[nodiscard]] bool hasConflicts() const { return !conflicts().empty(); }
    /// @brief Verifies a result (the four-eyes rule applies). @param resultId The result.
    void verify(std::int64_t resultId);
    /// @brief Tracked. @return Whether this tab's handler is attached to the open sample.
    [[nodiscard]] bool attached() const { return _attach.value().has_value(); }
    /// @brief Tracked. @return `"verified result <id>"` after a verification, or empty.
    [[nodiscard]] std::string statusText() const;
    /// @brief Tracked. @return The first failure, or empty.
    [[nodiscard]] std::string errorText() const;

private:
    [[nodiscard]] morph::async::Completion<std::string> submit(std::string_view actionType, std::string body);

    morph::examples::Wiring _wiring;
    SampleController const* _samples;
    morph::bridge::BridgeHandler<AnalysisCatalogModel> _catalog;
    morph::bridge::BridgeHandler<SampleModel, morph::bridge::AllowShared> _sample;
    morph::reactive::Signal<std::optional<std::int64_t>> _picked;
    morph::reactive::Query<ListAnalyses> _analyses;
    morph::reactive::Query<OpenSample> _attach;
    morph::reactive::Query<ListResults> _results;
    morph::reactive::Query<ListConflicts> _conflicts;
    morph::reactive::Mutation<VerifyResult> _verify;
    morph::forms::FormSession _capture;
    morph::forms::FormSession _resolve;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    morph::async::CallbackScope _lifetime;
};

}  // namespace lims::client
```

Create `examples/lims/app/controllers/result_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/result_controller.hpp"

#include <exception>
#include <morph/forms/engine/field_model.hpp>
#include <utility>

#include "app/completion_map.hpp"

namespace lims::client {

ResultController::ResultController(morph::examples::Wiring wiring, SampleController& samples)
    : _wiring{wiring},
      _samples{&samples},
      _catalog{wiring.bridge, &wiring.callbacks},
      _sample{wiring.bridge, &wiring.callbacks},
      _picked{wiring.runtime, std::nullopt},
      _analyses{wiring.runtime, _catalog, []() -> std::optional<ListAnalyses> { return ListAnalyses{}; }},
      _attach{wiring.runtime, _sample,
              [this]() -> std::optional<OpenSample> {
                  std::optional<SampleId> const sample = _samples->sampleId();
                  if (!sample) {
                      return std::nullopt;
                  }
                  return OpenSample{.sampleId = *sample};
              }},
      _results{wiring.runtime, _sample,
               [this]() -> std::optional<ListResults> {
                   if (!_attach.value()) {
                       return std::nullopt;
                   }
                   return ListResults{};
               }},
      _conflicts{wiring.runtime, _sample,
                 [this]() -> std::optional<ListConflicts> {
                     if (!_attach.value()) {
                         return std::nullopt;
                     }
                     return ListConflicts{};
                 }},
      _verify{wiring.runtime, _sample, morph::reactive::MutationOptions{.invalidates = {&_results}}},
      _capture{wiring.runtime, morph::forms::FormModel::forAction<CaptureConcentration>(),
               [this](std::string_view actionType, std::string body) { return submit(actionType, std::move(body)); },
               // The lists name no sample, so they come from the plain handler: this tab's shared one is not bound
               // until it attaches, and the form fetches them as it is constructed.
               morph::forms::handlerChoiceFetcher(wiring.callbacks, samples.registrar())},
      _resolve{wiring.runtime, morph::forms::FormModel::forAction<ResolveConflict>(),
               [this](std::string_view actionType, std::string body) { return submit(actionType, std::move(body)); },
               {}} {}

std::vector<AnalysisRow> ResultController::analyses() const {
    std::vector<AnalysisRow> rows;
    if (auto const& listed = _analyses.value()) {
        for (AnalysisVersionView const& view : listed->analyses) {
            rows.push_back(analysisRow(view));
        }
    }
    return rows;
}

std::string ResultController::pickedText() const {
    auto const& picked = _picked.get();
    return picked ? "version id " + std::to_string(*picked) : std::string{};
}

std::vector<ResultRow> ResultController::results() const {
    std::vector<ResultRow> rows;
    if (auto const& listed = _results.value()) {
        for (ResultView const& view : listed->results) {
            rows.push_back(resultRow(view));
        }
    }
    return rows;
}

std::vector<ConflictRow> ResultController::conflicts() const {
    std::vector<ConflictRow> rows;
    if (auto const& listed = _conflicts.value()) {
        for (ConflictView const& view : listed->conflicts) {
            rows.push_back(conflictRow(view));
        }
    }
    return rows;
}

void ResultController::verify(std::int64_t resultId) { _verify.run(VerifyResult{.resultId = ResultId{resultId}}); }

std::string ResultController::statusText() const {
    auto const& verified = _verify.lastResult();
    if (!verified || !verified->resultId.hasValue()) {
        return {};
    }
    return "verified result " + std::to_string(*verified->resultId);
}

std::string ResultController::errorText() const {
    for (std::exception_ptr const& error :
         {_verify.error(), _attach.error(), _results.error(), _conflicts.error(), _analyses.error()}) {
        if (error != nullptr) {
            return morph::reactive::errorMessage(error);
        }
    }
    return {};
}

morph::async::Completion<std::string> ResultController::submit(std::string_view actionType, std::string body) {
    bool const capture = actionType == morph::model::ActionTraits<CaptureConcentration>::typeId();
    return morph::examples::mapCompletion<std::string>(
        _wiring.callbacks, _lifetime.token(), _sample.executeJson(actionType, body),
        [this, capture](std::string const& reply) {
            (capture ? _capture : _resolve).reset();
            _results.refetch();
            _conflicts.refetch();
            return reply;
        },
        [](std::exception_ptr const&) {});
}

}  // namespace lims::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_lims_tests
./build/all/examples/lims/ladder_lims_tests "[lims][client]"
```

Expected: PASS.

Mutation check: key `_attach` on nothing (`return std::nullopt;` always). Expected FAIL: "follows the lifecycle
tab's sample" times out on `attached()`, and every capture case is refused because the shared handler never attaches.
Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/lims/app/controllers/result_controller.hpp examples/lims/app/controllers/result_controller.cpp \
        examples/lims/tests/client/test_result_controller.cpp
git commit -m "wip(lims): ResultController: catalogue, capture, results, verify and conflicts on the shared sample

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task M4: lims' `AppController` and views — two tabs under a header

`Main.qml`'s header row, `TabBar` + `StackLayout` and error label become a `Column{header Row, Tabs, error Text}`.
`SampleView.qml`'s and `ResultEntryView.qml`'s `DynamicForm`s become `forms::formView` over the controllers'
runtime sessions (each action is `explicitSubmit`, so each form carries its own submit button, labelled here). The
return/reject forms, which QML disabled outside their states, are hidden outside them: a `Panel`'s `enabled` is not
specified to reach its children, its `visible` is.

**Files:**
- Create: `examples/lims/app/app_controller.hpp`, `examples/lims/app/app_controller.cpp`
- Create: `examples/lims/app/views/lims_views.hpp`, `examples/lims/app/views/lims_views.cpp`
- Test: `examples/lims/tests/client/test_lims_views.cpp`

**Interfaces:**
- Consumes: Tasks M2, M3; Part 2's `ui::column`, `row`, `panel`, `tabs`, `table<RowT>`, `forEach<RowT>`,
  `textInput`, `select`, `button`, `text`, `spacer`; Part 5's `forms::formView`, `FormViewOptions{.submitLabel}`;
  `RecordingBackend::find`, `prop`, `click`, `edit`, `dump` (a `Tabs` page is mounted when its tab is first
  selected); `lims::testing::fill` (Task M2).
- Produces: `lims::client::AppController(examples::Wiring)` with `samples()`, `results()`, `headerText()`, `errorText()`,
  `tab()`, `selectTab(std::size_t)`; views `lifecycleTab(SampleController&)`, `resultsTab(ResultController&)`,
  `rootView(AppController&)`.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/client/test_lims_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// One RecordingBackend test per screen: the tree shows what the screen must, and its controls drive the controller.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "app_controller.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims_client_support.hpp"
#include "testkit/db_fixture.hpp"
#include "views/lims_views.hpp"

namespace {

using lims::client::AppController;
using lims::testing::LocalClient;
using morph::ladder::testkit::DbFixture;
using morph::ui::testing::RecordingBackend;

[[nodiscard]] bool shows(RecordingBackend const& backend, std::string const& text) {
    return backend.dump().find(text) != std::string::npos;
}

[[nodiscard]] int byLabel(RecordingBackend const& backend, std::string const& label) {
    auto const found = backend.find("Button", "label", label);
    REQUIRE(found.has_value());
    return *found;
}

// Clicks the button labelled @p label once it is enabled. A form's Submit button follows `ready()`, and a
// transition's button its controller's `can…()`, one flush after the change; the backend ignores a click on a
// disabled widget.
void clickWhenEnabled(LocalClient& client, RecordingBackend& backend, std::string const& label) {
    int const target = byLabel(backend, label);
    REQUIRE(client.settle([&backend, target] { return backend.prop(target, "enabled") != "false"; }));
    backend.click(target);
}

}  // namespace

TEST_CASE("lims view: the shell shows the header, both tabs, and follows the attached sample", "[lims][view]") {
    DbFixture const fixture;
    LocalClient client{"alice"};
    AppController app{client.wiring()};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, lims::client::rootView(app)};
    CHECK(shows(backend, "no sample attached"));
    // Both labels are the Tabs widget's own; only the selected (lifecycle) page is mounted yet.
    CHECK(backend.find("Tabs", "tabs", "[Lifecycle,Results]").has_value());
    CHECK(backend.find("Button", "label", "Register client").has_value());
    CHECK_FALSE(backend.find("Button", "label", "Refresh catalogue").has_value());

    lims::testing::registerSample(client, app.samples());

    REQUIRE(client.settle([&backend] { return shows(backend, "registered (v"); }));
    CHECK_FALSE(shows(backend, "no sample attached"));
}

TEST_CASE("lims view: the lifecycle tab registers through its forms and moves the sample", "[lims][view]") {
    DbFixture const fixture;
    LocalClient client{"alice"};
    AppController app{client.wiring()};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, lims::client::lifecycleTab(app.samples())};
    CHECK(shows(backend, "Latest client id: -1"));

    lims::testing::fill(app.samples().registerClientForm(), R"({"name":"Waterworks Ltd"})");
    clickWhenEnabled(client, backend, "Register client");
    REQUIRE(client.settle([&backend] { return !shows(backend, "Latest client id: -1"); }));

    lims::testing::fill(app.samples().registerSampleForm(), R"({"reference":"WW-1"})");
    clickWhenEnabled(client, backend, "Register sample");
    REQUIRE(client.settle([&app] { return app.samples().state() == "registered"; }));

    clickWhenEnabled(client, backend, "Receive");
    REQUIRE(client.settle([&app] { return app.samples().state() == "received"; }));
    CHECK(shows(backend, "state: received"));
}

TEST_CASE("lims view: the results tab lists the catalogue, captures, and reports a refused verify", "[lims][view]") {
    DbFixture const fixture;
    lims::AnalysisVersionId nitrate;
    {
        morph::session::Context context;
        context.principal = "alice";
        morph::session::detail::ScopedContext const scope{context};
        lims::AnalysisCatalogModel catalog;
        nitrate = catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3})
                      .versionId;
    }
    LocalClient client{"alice"};
    AppController app{client.wiring()};
    lims::testing::registerSample(client, app.samples());
    app.samples().receive();
    REQUIRE(client.settle([&app] { return app.samples().state() == "received"; }));
    app.samples().startWork();
    REQUIRE(client.settle([&app] { return app.samples().state() == "in-progress"; }));
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, lims::client::resultsTab(app.results())};

    backend.click(byLabel(backend, "Refresh catalogue"));
    REQUIRE(client.settle([&backend] { return shows(backend, "Nitrate"); }));

    auto const body = glz::write_json(lims::CaptureConcentration{
        .analysisVersionId = nitrate,
        .value = lims::Concentration{morph::math::Rational{morph::math::Numerator{12}, morph::math::Denominator{5},
                                                           morph::math::DecimalPlaces{3}}}});
    REQUIRE(body.has_value());
    lims::testing::fill(app.results().captureForm(), *body);
    clickWhenEnabled(client, backend, "Capture");
    REQUIRE(client.settle([&backend] { return shows(backend, "2.4 mg/L"); }));

    backend.click(byLabel(backend, "Verify"));
    REQUIRE(client.settle([&app] { return !app.results().errorText().empty(); }));
    CHECK(shows(backend, app.results().errorText()));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_lims_tests`
Expected: compile error, `'app_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/lims/app/app_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/reactive/signal.hpp>
#include <string>

#include "app/wiring.hpp"
#include "controllers/result_controller.hpp"
#include "controllers/sample_controller.hpp"

namespace lims::client {

/// @brief The lims client: the lifecycle and results tabs' controllers and the selected tab.
class AppController {
public:
    /// @param wiring The runtime, bridge and owner both controllers use.
    explicit AppController(morph::examples::Wiring wiring);

    /// @brief The lifecycle tab's controller. @return It.
    [[nodiscard]] SampleController& samples() noexcept { return _samples; }
    /// @brief The results tab's controller. @return It.
    [[nodiscard]] ResultController& results() noexcept { return _results; }
    /// @brief Tracked. @return The header line naming the attached sample.
    [[nodiscard]] std::string headerText() const { return _samples.headerText(); }
    /// @brief Tracked. @return The lifecycle tab's failure, shown under both tabs, or empty.
    [[nodiscard]] std::string errorText() const { return _samples.errorText(); }
    /// @brief The selected tab. Tracked. @return Its index.
    [[nodiscard]] std::size_t tab() const { return _tab.get(); }
    /// @brief Selects a tab. @param index Its index.
    void selectTab(std::size_t index) { _tab.set(index); }

private:
    SampleController _samples;
    ResultController _results;  // borrows _samples, so it is declared after it
    morph::reactive::Signal<std::size_t> _tab;
};

}  // namespace lims::client
```

Create `examples/lims/app/app_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "app_controller.hpp"

namespace lims::client {

AppController::AppController(morph::examples::Wiring wiring)
    : _samples{wiring}, _results{wiring, _samples}, _tab{wiring.runtime, std::size_t{0}} {}

}  // namespace lims::client
```

Create `examples/lims/app/views/lims_views.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "app_controller.hpp"

namespace lims::client {

/// @brief The lifecycle tab. @param samples Its controller. @return The tab's content.
[[nodiscard]] morph::ui::Node lifecycleTab(SampleController& samples);
/// @brief The results tab. @param results Its controller. @return The tab's content.
[[nodiscard]] morph::ui::Node resultsTab(ResultController& results);
/// @brief The whole app: the header, both tabs, and the lifecycle tab's failure.
/// @param app The app; it outlives the mounted view.
/// @return The root node.
[[nodiscard]] morph::ui::Node rootView(AppController& app);

}  // namespace lims::client
```

Create `examples/lims/app/views/lims_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/lims_views.hpp"

#include <cstdint>
#include <functional>
#include <morph/forms/engine/form_view.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lims::client {

namespace {

namespace ui = morph::ui;
using morph::reactive::Signal;

ui::Node form(morph::forms::FormSession& session, std::string submitLabel) {
    return morph::forms::formView(session, morph::forms::FormViewOptions{.submitLabel = std::move(submitLabel)});
}

ui::Node stretchText(std::function<std::string()> text, ui::TextRole role = ui::TextRole::Normal) {
    return ui::text({.text = std::move(text), .role = role, .common = {.layout = {.width = ui::Sizing::stretch()}}});
}

std::vector<ui::SelectOption> analysisOptions(ResultController const& results) {
    std::vector<ui::SelectOption> options;
    for (AnalysisRow const& row : results.analyses()) {
        options.push_back(ui::SelectOption{.key = ui::Key{row.versionId}, .label = row.name});
    }
    return options;
}

}  // namespace

ui::Node lifecycleTab(SampleController& samples) {
    return ui::column({
        .children =
            {
                ui::panel({.title = "Register",
                           .padding = 1,
                           .child = ui::column({.children = {form(samples.registerClientForm(), "Register client"),
                                                             ui::text({.text = [&samples] { return samples.clientText(); },
                                                                       .role = ui::TextRole::Muted}),
                                                             form(samples.registerSampleForm(), "Register sample")},
                                                .gap = 1})}),
                ui::row({.children = {ui::text({.text = "Open existing"}),
                                      ui::textInput({.value = [&samples] { return samples.openIdText(); },
                                                     .onChange = [&samples](std::string text) { samples.setOpenIdText(std::move(text)); },
                                                     .placeholder = "Sample id"}),
                                      ui::button({.label = "Open sample",
                                                  .onClick = [&samples] { samples.openTyped(); },
                                                  .common = {.enabled = [&samples] { return samples.canOpen(); }}})},
                         .gap = 1}),
                ui::panel(
                    {.title = "Lifecycle",
                     .padding = 1,
                     .child = ui::column(
                         {.children =
                              {ui::text({.text = [&samples] { return "state: " + samples.state(); },
                                         .common = {.visible = [&samples] { return samples.sample().has_value(); }}}),
                               ui::row({.children = {ui::button({.label = "Receive",
                                                                 .onClick = [&samples] { samples.receive(); },
                                                                 .common = {.enabled = [&samples] { return samples.canReceive(); }}}),
                                                     ui::button({.label = "Start work",
                                                                 .onClick = [&samples] { samples.startWork(); },
                                                                 .common = {.enabled = [&samples] { return samples.canStartWork(); }}}),
                                                     ui::button({.label = "Submit for verification",
                                                                 .onClick = [&samples] { samples.submitForVerification(); },
                                                                 .common = {.enabled = [&samples] { return samples.canSubmit(); }}}),
                                                     ui::button({.label = "Publish",
                                                                 .onClick = [&samples] { samples.publish(); },
                                                                 .common = {.enabled = [&samples] { return samples.canPublish(); }}}),
                                                     ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                                     ui::button({.label = "Refresh",
                                                                 .onClick = [&samples] { samples.refresh(); },
                                                                 .common = {.enabled = [&samples] { return samples.sample().has_value(); }}})},
                                        .gap = 1})},
                          .gap = 1})}),
                ui::panel({.title = "Return or reject",
                           .padding = 1,
                           .child = ui::column(
                               {.children = {ui::column({.children = {form(samples.reworkForm(), "Return for rework")},
                                                         .common = {.visible = [&samples] { return samples.canRework(); }}}),
                                             ui::column({.children = {form(samples.rejectForm(), "Reject")},
                                                         .common = {.visible = [&samples] { return samples.canReject(); }}})},
                                .gap = 1}),
                           .common = {.visible = [&samples] { return samples.canRework() || samples.canReject(); }}}),
            },
        .gap = 1,
    });
}

ui::Node resultsTab(ResultController& results) {
    return ui::column({
        .children =
            {
                ui::row({.children = {ui::text({.text = "Catalogue"}),
                                      ui::select({.options = [&results] { return analysisOptions(results); },
                                                  .selected = [&results]() -> std::optional<ui::Key> {
                                                      auto const& picked = results.picked();
                                                      return picked ? std::optional<ui::Key>{ui::Key{*picked}} : std::nullopt;
                                                  },
                                                  .onSelect =
                                                      [&results](ui::Key const& key) {
                                                          if (auto const* const version = std::get_if<std::int64_t>(&key);
                                                              version != nullptr) {
                                                              results.pick(*version);
                                                          }
                                                      }}),
                                      ui::text({.text = [&results] { return results.pickedText(); }, .role = ui::TextRole::Muted}),
                                      ui::button({.label = "Refresh catalogue", .onClick = [&results] { results.refreshAnalyses(); }})},
                         .gap = 1}),
                ui::panel({.title = "Capture", .padding = 1, .child = form(results.captureForm(), "Capture")}),
                ui::row({.children = {ui::text({.text = "Results", .role = ui::TextRole::Heading}),
                                      ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                      ui::button({.label = "Refresh results",
                                                  .onClick = [&results] { results.refreshResults(); },
                                                  .common = {.enabled = [&results] { return results.attached(); }}}),
                                      ui::button({.label = "Refresh conflicts",
                                                  .onClick = [&results] { results.refreshConflicts(); },
                                                  .common = {.enabled = [&results] { return results.attached(); }}})},
                         .gap = 1}),
                ui::table<ResultRow>(
                    {{.label = "Id"},
                     {.label = "Reading", .width = ui::Sizing::stretch()},
                     {.label = "Spec"},
                     {.label = "Captured by"},
                     {.label = ""}},
                    [&results] { return results.results(); }, [](ResultRow const& row) { return ui::Key{row.id}; },
                    [&results](Signal<ResultRow> const& row) {
                        return std::vector<ui::Node>{
                            ui::text({.text = [&row] { return row.get().idText; }, .role = ui::TextRole::Muted}),
                            ui::text({.text = [&row] { return row.get().reading; }}),
                            ui::text({.text = [&row] { return row.get().outOfSpec; }, .role = ui::TextRole::Error}),
                            ui::text({.text = [&row] { return row.get().capturedBy; }, .role = ui::TextRole::Muted}),
                            ui::button({.label = "Verify", .onClick = [&results, &row] { results.verify(row.peek().id); }})};
                    }),
                ui::forEach<ConflictRow>(
                    [&results] { return results.conflicts(); }, [](ConflictRow const& row) { return ui::Key{row.id}; },
                    [](Signal<ConflictRow> const& row) {
                        return stretchText([&row] { return row.get().text; }, ui::TextRole::Error);
                    }),
                ui::panel({.title = "Resolve conflict",
                           .padding = 1,
                           .child = form(results.resolveForm(), "Resolve"),
                           .common = {.visible = [&results] { return results.hasConflicts(); }}}),
                ui::text({.text = [&results] { return results.statusText(); }, .role = ui::TextRole::Success}),
                ui::text({.text = [&results] { return results.errorText(); }, .role = ui::TextRole::Error}),
            },
        .gap = 1,
    });
}

ui::Node rootView(AppController& app) {
    return ui::column({
        .children =
            {
                ui::row({.children = {ui::text({.text = "lims", .role = ui::TextRole::Heading}),
                                      stretchText([&app] { return app.headerText(); }, ui::TextRole::Muted)},
                         .gap = 1}),
                ui::tabs({.tabs = {{.label = "Lifecycle", .node = lifecycleTab(app.samples())},
                                   {.label = "Results", .node = resultsTab(app.results())}},
                          .selected = [&app] { return app.tab(); },
                          .onSelect = [&app](std::size_t index) { app.selectTab(index); },
                          .common = {.layout = {.height = ui::Sizing::stretch()}}}),
                ui::text({.text = [&app] { return app.errorText(); }, .role = ui::TextRole::Error}),
            },
        .gap = 1,
    });
}

}  // namespace lims::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_lims_tests
./build/all/examples/lims/ladder_lims_tests "[lims][view]"
git grep -n -E '#include <Q|morph/qt|morph/tui|morph/qt_quick' -- examples/lims/app; test $? -eq 1 && echo ok-app
git grep -n 'morph/ui/' -- examples/lims/app/controllers; test $? -eq 1 && echo ok-controllers
```

Expected: PASS (3 cases); `ok-app`, `ok-controllers`.

Mutation check: in `resultsTab`, bind the Verify button's `onClick` to `results.refreshResults()`. Expected FAIL in
"the results tab lists the catalogue, captures, and reports a refused verify" (the error never appears). Restore it.

- [ ] **Step 5: Commit**

```bash
git add examples/lims/app/app_controller.hpp examples/lims/app/app_controller.cpp examples/lims/app/views \
        examples/lims/tests/client/test_lims_views.cpp
git commit -m "wip(lims): AppController and the lifecycle and results tab views

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task M5: lims' application, binary and frontend smoke tests

lims still has no server of its own. The client runs its models in-process by default, exactly as the old
`ladder_lims_gui` did; `--server` is kept because `connect` offers it and a deployment may host `SampleModel` and
`AnalysisCatalogModel` itself. Unlike the old client, it installs a principal: `--user`, default `demo`, so a capture
records who captured it. Its database is `--db`, else the `LIMS_DB` environment variable (read through
`ui::processEnvironment()`), else `lims.db`, so `ui/main.cpp` is exactly spec 4 §3's composition root.

**Files:**
- Create: `examples/lims/app/lims_application.hpp`, `examples/lims/app/lims_application.cpp`
- Create: `examples/lims/ui/main.cpp`
- Test: `examples/lims/tests/smoke/test_lims_frontends.cpp`

**Interfaces:**
- Consumes: as Task L9 (Part 6's `AppEnvironment`, `connect`, `LocalSetup`, `Connection`, `Wiring`; Part 2's
  frontend seam and `ui::processEnvironment()`; Parts 3–4's `frontendOption`s; `runFrontendSmoke`);
  `AppController`, `rootView` (Task M4); `lims::db::setup`; `Bridge::setDefaultSession` (`morph/core/bridge.hpp`, as
  `LocalClient` in Task M2 uses it).
- Produces: `lims::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&) ->
  std::unique_ptr<ui::Application>`; the `lims` binary.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/smoke/test_lims_frontends.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The application mounts and quits on every frontend this build has (spec 4 §7 rule 5). This file is
// ladder_lims_smoke_tests, a binary of its own: its main owns no Qt application object.

#if MORPH_EXAMPLE_HAS_TUI || MORPH_EXAMPLE_HAS_QT_QUICK

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"
#include "lims_application.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/frontend_smoke.hpp"

namespace {

using morph::examples::testing::SmokeFrontend;

void smoke(SmokeFrontend frontend) {
    morph::ladder::testkit::DbFixture const fixture;
    morph::examples::AppEnvironment env;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read on the test thread before the app starts any thread.
    env.db = morph::ladder::testkit::DbFixture::computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"));
    env.user = "alice";
    morph::examples::testing::runFrontendSmoke(
        [&env](morph::ui::AppContext& ctx) { return lims::client::makeApplication(ctx, env); }, frontend);
}

}  // namespace

#if MORPH_EXAMPLE_HAS_TUI
TEST_CASE("lims mounts and quits on the TUI", "[lims][smoke]") { smoke(SmokeFrontend::Tui); }
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
TEST_CASE("lims mounts and quits on Qt Quick", "[lims][smoke]") { smoke(SmokeFrontend::QtQuick); }
#endif

#endif
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake build/all && cmake --build build/all --target ladder_lims_smoke_tests`.
Expected: compile error, `'lims_application.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/lims/app/lims_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"

namespace lims::client {

/// @brief Builds the lims application on a frontend's context: hosts the models in-process on `--db` (else the
///        `LIMS_DB` environment variable, else `lims.db`), or reaches them at `--server`, and acts as `--user`
///        (default `demo`).
/// @param ctx The frontend's context. Borrowed: it outlives the application.
/// @param env `--server`, `--db`, `--user`.
/// @return The application.
/// @throws morph::examples::TransportError when `--server` is given and this frontend has no transport for it.
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env);

}  // namespace lims::client
```

Create `examples/lims/app/lims_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims_application.hpp"

#include <cstddef>
#include <morph/session/session.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "app/transport.hpp"
#include "app/wiring.hpp"
#include "app_controller.hpp"
#include "lims/db/database.hpp"
#include "views/lims_views.hpp"

namespace lims::client {

namespace {

constexpr std::string_view kDefaultDatabase = "DRIVER=SQLite3;Database=lims.db;Timeout=5000";
constexpr std::string_view kDevPrincipal = "demo";
constexpr std::size_t kLocalWorkers = 4;

[[nodiscard]] morph::examples::AppEnvironment withDatabase(morph::examples::AppEnvironment env) {
    if (env.db.empty()) {
        env.db = morph::ui::processEnvironment()("LIMS_DB").value_or(std::string{kDefaultDatabase});
    }
    return env;
}

// The models re-check every role they depend on, on every dispatch path, so an in-process client is gated as hard
// as a remote one. What it needs is a principal for those checks and for the records' "captured by".
[[nodiscard]] std::unique_ptr<morph::examples::Connection> actingAs(
    std::unique_ptr<morph::examples::Connection> connection, std::string principal) {
    morph::session::Context session;
    session.principal = std::move(principal);
    connection->bridge().setDefaultSession(std::move(session));
    return connection;
}

class LimsApplication final : public morph::ui::Application {
public:
    LimsApplication(morph::ui::AppContext& ctx, morph::examples::AppEnvironment const& env)
        : _connection{actingAs(
              morph::examples::connect(
                  ctx, withDatabase(env),
                  morph::examples::LocalSetup{.setupDatabase = [](std::string const& database) { db::setup(database); },
                                              .workers = kLocalWorkers}),
              env.user.empty() ? std::string{kDevPrincipal} : env.user)},
          _app{morph::examples::Wiring{.runtime = ctx.runtime(),
                                       .scheduler = ctx.scheduler(),
                                       .bridge = _connection->bridge(),
                                       .callbacks = _connection->callbacks()}} {}

    [[nodiscard]] morph::ui::Node view() override { return rootView(_app); }

private:
    std::unique_ptr<morph::examples::Connection> _connection;
    AppController _app;
};

}  // namespace

std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                        morph::examples::AppEnvironment const& env) {
    return std::make_unique<LimsApplication>(ctx, env);
}

}  // namespace lims::client
```

Create `examples/lims/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The lims client: picks the Qt Quick or terminal frontend at runtime (--ui=qt|tui, MORPH_UI, or the first that can
// run here) and hands it the application.
//
//   lims [--ui=qt|tui] [--db <odbc>] [--user <name>] [--server ws://host:port]
//
// Without --db, the LIMS_DB environment variable names the database, else lims.db.

#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <vector>

#include "app/app_environment.hpp"
#include "lims_application.hpp"

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#endif
#if MORPH_EXAMPLE_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

int main(int argc, char** argv) {
    try {
        auto const env = morph::examples::AppEnvironment::fromArgs(argc, argv);
        std::vector<morph::ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
        built.push_back(morph::qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
        built.push_back(morph::tui::frontendOption());
#endif
        auto const frontend = morph::ui::selectFrontend(built, argc, argv);
        return frontend->run([&env](morph::ui::AppContext& ctx) { return lims::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "lims: " << error.what() << '\n';
        return 1;
    }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_lims_smoke_tests lims
./build/all/examples/lims/ladder_lims_smoke_tests
```

Expected: both targets build; 2 smoke cases pass.

Mutation check: make `makeApplication` begin with `throw std::runtime_error{"mutation"};`. Expected FAIL in both smoke
cases. Restore it.

By hand, once: `./build/all/examples/lims/lims --ui=tui --db "DRIVER=SQLite3;Database=/tmp/lims-manual.db"` —
register a client and a sample, receive it, start work, switch to Results, Ctrl+C; the same with `--ui=qt`. Report
what you saw.

- [ ] **Step 5: Commit**

```bash
git add examples/lims/app/lims_application.hpp examples/lims/app/lims_application.cpp examples/lims/ui \
        examples/lims/tests/smoke/test_lims_frontends.cpp
git commit -m "wip(lims): the application and one binary for both frontends

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task M6: Retire lims' QML stack; the README's client section

| Deleted test file | What it pinned | Where that claim lives now |
|---|---|---|
| `test_lims_presenters.cpp` | registration and every transition routed; a refusal displayable; busy while in flight; catalogue, capture and listing routed | `test_sample_controller.cpp` (the first three, one case each), `test_result_controller.cpp` ("a captured result carries the exact decimal") |
| `test_lims_qml_bridges.cpp` | every connected signal exists; the sample bag carries every key QML reads | by construction: views call controller members, and the compiler checks every one |
| | a refusal reaches the operator and `lastError` holds it | `test_sample_controller.cpp` ("surfaces a model refusal…"), `test_result_controller.cpp` ("verifying one's own result is refused…") |
| | a result row's exact decimal; a no-number result names its qualifier | `test_result_controller.cpp` (two cases), `test_lims_format.cpp` ("a result row names its reading…") |
| | an unengaged id crosses as -1; quantity text exact and empty-safe | `test_sample_controller.cpp` (`"Latest client id: -1"`), `test_lims_format.cpp` ("quantity text is exact and empty-safe") |
| | the schema document carries a form per typed-field action; `submitIfValid` refuses a foreign action | by construction: each controller builds one `FormSession` per action from that action's own schema, and a session submits only its own action type |
| | a form body reaches the model; registering through forms leaves the shared handler attached with the client id set; a model's refusal of a body is its own message | `test_sample_controller.cpp` ("routes registration…", "the reject form's refusal is the model's own message"), `test_result_controller.cpp` ("follows the lifecycle tab's sample") |
| `test_lims_qml_surface.cpp` | the bridges expose exactly what QML binds; a refused form submission has a path to the operator | by construction; `formView` shows each session's last error itself (Part 5), and `test_lims_views.cpp` shows the results tab's refused verify |
| | every rendered form opts out of auto-submit; a read-only action's schema does not | `test_lims_forms.cpp` (below) |
| `test_gui_qml_smoke.cpp` | `Main.qml` and both surfaces load; each schema form renders its own Submit button | `tests/smoke/test_lims_frontends.cpp`; `test_lims_views.cpp` clicks "Register client", "Register sample" and "Capture" |

**Files:**
- Delete: `examples/lims/gui/`, `examples/lims/gui_lib/`, and in `examples/lims/tests/`:
  `test_lims_presenters.cpp`, `test_lims_qml_bridges.cpp`, `test_lims_qml_surface.cpp`, `test_gui_qml_smoke.cpp`
- Create: `examples/lims/tests/client/test_lims_forms.cpp`
- Modify: `examples/lims/README.md`, `examples/lims/src/models/sample_model.cpp` (one comment),
  `examples/crm/include/crm/gui/crm_schemas.hpp` (one comment), `codecov.yml`
- Test: the whole lims suite, plus the check below

**Interfaces:** none new.

- [ ] **Step 1: Write the failing check and the replacement test**

```bash
test ! -e examples/lims/gui && test ! -e examples/lims/gui_lib && \
  { git grep -n -E 'lims::gui|lims_qml|ladder_lims_gui|lims/gui_lib|lims/gui/|gui/qml|lims_schemas|SampleBridge|ResultBridge|SamplePresenter|ResultPresenter' -- \
      examples/lims examples/crm/include/crm/gui/crm_schemas.hpp codecov.yml; test $? -eq 1; } && \
  echo "lims' QML stack is gone"
```

Create `examples/lims/tests/client/test_lims_forms.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Every form the lims client renders has effects (a registration, a rework or rejection, a reading, a conflict
// resolution), so each must wait for its Submit button: a form that auto-submitted would file one action per
// keystroke. The served schema is what decides it, so this reads the session's model, not the DTO's declaration.

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/field_model.hpp>

#include "controllers/result_controller.hpp"
#include "controllers/sample_controller.hpp"
#include "lims_client_support.hpp"
#include "testkit/db_fixture.hpp"

using morph::forms::SubmitMode;

TEST_CASE("lims::client: every form the client renders submits only on its button", "[lims][client]") {
    morph::ladder::testkit::DbFixture const fixture;
    lims::testing::LocalClient client{"alice"};
    lims::client::SampleController samples{client.wiring()};
    lims::client::ResultController results{client.wiring(), samples};

    for (morph::forms::FormSession const* form :
         {&samples.registerClientForm(), &samples.registerSampleForm(), &samples.rejectForm(), &samples.reworkForm(),
          &results.captureForm(), &results.resolveForm()}) {
        INFO("action type: " << form->model().actionType());
        CHECK(form->model().submitMode() == SubmitMode::Explicit);
    }
}

TEST_CASE("lims::client: a read-only action's form would submit automatically", "[lims][client]") {
    // The other half, and why the check above is not vacuous: explicit submit is opt-in, so a schema writer that
    // stamped it on every action would pass the first case too.
    CHECK(morph::forms::FormModel::forAction<lims::GetSample>().submitMode() == SubmitMode::Automatic);
}
```

- [ ] **Step 2: Run them to verify the check fails**

Expected: no "gone" line (the directories exist; the README, `sample_model.cpp`, `crm_schemas.hpp` and
`codecov.yml` match). `test_lims_forms.cpp` already passes: it pins behaviour the controllers have since Tasks M2 and
M3, and is added here because it replaces a deleted test's claim.

- [ ] **Step 3: Implement**

```bash
git rm -r -q examples/lims/gui examples/lims/gui_lib
git rm -q examples/lims/tests/test_lims_presenters.cpp examples/lims/tests/test_lims_qml_bridges.cpp \
          examples/lims/tests/test_lims_qml_surface.cpp examples/lims/tests/test_gui_qml_smoke.cpp
```

`examples/lims/src/models/sample_model.cpp`, the comment above `markDecided`: replace its second paragraph ("A free
function rather than a member: … is ORM-free for the same reason.") with:

```cpp
/// A free function rather than a member: it touches no state of its own, and
/// keeping `Lightweight::DataMapper` out of `sample_model.hpp` keeps that
/// header includable by a client built without the ORM (`MORPH_CLIENT_ONLY`).
/// Every other rung's model header is ORM-free for the same reason.
```

`examples/crm/include/crm/gui/crm_schemas.hpp`, the `@file` comment: replace "mirrors
`lims::gui::limsSchemasJson()`'s exact shape (`examples/lims/gui_lib/lims_schemas.hpp`). Lives under
`include/crm/gui` rather than `gui_lib/` (unlike lims's copy): it has" with "one entry per form action, each
`schemaJson<A>()`. Lives under `include/crm/gui` rather than `gui_lib/`: it has", and "Selection rule (from lims's
file, reused here):" with "Selection rule:", and "for the same reason lims excludes its own empty-body transitions"
with "because an action with no fields has no form to generate".

`codecov.yml`: replace `"examples/lims/gui/**"` and `"examples/lims/gui_wasm/**"` with `"examples/lims/ui/**"`.

`examples/lims/README.md`:

1. In the status paragraph, replace "and a full QML GUI under [`gui/qml/`](gui/qml);" with "and a client for Qt Quick
   and the terminal under [`app/`](app);". In "Resolved design decisions", replace "and a full QML GUI under
   [`gui/qml/`](gui/qml)." with "and a client for Qt Quick and the terminal under [`app/`](app).".
2. Replace everything from `## The client` up to (not including) `### 20. Model coverage is 98.96%` with:

```markdown
## The client

One binary, `lims` (`ui/main.cpp`), runs on Qt Quick or in a terminal: `--ui=qt|tui`, else `MORPH_UI`, else the
first frontend that can run here. It hosts the models in-process on `--db` (else `LIMS_DB`, else `lims.db`) and acts
as `--user` (default `demo`); the models re-check every role they depend on, on every dispatch path, so an
in-process client is gated as hard as a remote one. `--server ws://host:port` reaches the same models hosted
elsewhere. **Known gap:** this rung builds no server of its own, so `--server` needs one somebody else hosts.

`app/` is `ladder_lims_app`, which links no toolkit: `SampleController` (the lifecycle tab), `ResultController`
(the results tab), `AppController` (both, and the selected tab) and one view per tab, made of bindings.

Two tabs, one shell: the sample lifecycle and result entry, side by side rather than stacked, because both act on
the *same* attached sample at once — a bench operator captures a reading while the office watches the state move.
The results tab's handler is `AllowShared` over the keyed `SampleModel` and attaches with a `Query<OpenSample>` keyed
on the lifecycle tab's attached sample id, so both land on one instance. That is the shared-instance design made
visible rather than asserted.

### Every typed field is schema-driven; nothing is hand-built

> **A field a person types is rendered from the served schema. A value the model already supplied is a typed
> call.**

So `RegisterClient`, `RegisterSample`, `RejectSample`, `ReturnForRework`, `CaptureConcentration` and
`ResolveConflict` are runtime forms: a `FormSession` built from the action's own `schemaJson<A>()`, rendered by
`forms::formView`. The zero-field transitions (`ReceiveSample`, `StartWork`, `SubmitForVerification`,
`PublishSample`) are plain buttons, because an action with no fields has no form to generate. `VerifyResult` and
`OpenSample` are typed calls: their one field is an id the table row or the id field already holds.

The capture form is the one that earns its keep: `exactlyOneOf(value, qualifier)` is the submit gate that makes "a
number *and* a below-LOD flag" unsubmittable; the `requiredWhen`/`visibleWhen` pair shows the dilution factor and
requires it exactly when the preparation says diluted; `x-decimalPlaces`/`x-unitAlternatives` drive the entry unit.
None of that is written in the app. Its qualifier and dilution lists come through `forms::handlerChoiceFetcher` over
the lifecycle tab's plain handler: they name no sample, and the results tab's shared handler cannot serve them before
it has attached.

Every one of these actions declares `explicitSubmit`, so each form waits for its own Submit button
(`tests/client/test_lims_forms.cpp`): an automatic form would file a lab reading mid-keystroke.

### A refusal is shown where it happened

A refusal is this rung's product, not its exception path — an over-precise reading (decision 7), an `exactlyOneOf`
violation (decision 6), a four-eyes refusal (decision 16), an unknown qualifier or dilution code (decision 18) and a
rejected conflict resolution are each a statement about the measurement that the analyst has to read. A form shows
its own last reply or the model's own message under its Submit button, and keeps what was typed until a submission
is accepted. The typed calls' failures — open, the transitions, verify — are each tab's error line; the lifecycle
tab's also shows under both tabs.

### Two handlers on the lifecycle tab, and why

`SampleModel` is keyed, so the handler every attached action runs on is `AllowShared`. But an `AllowShared` handler
is **not bound until it attaches to a key**, and `RegisterClient` carries none, so that one form submits on a
second, plain `NoSharing` handler, which registers a fresh instance on construction and is bound immediately.
`RegisterSample` needs no such help even though it too arrives before any key exists: it is result-keyed, so
`BridgeHandler::execute`'s `ResultKeyed` branch runs it on an anonymous instance and promotes that instance to the id
the result names before the completion resolves — one dispatch, on the *shared* handler, and that handler is
attached when it returns. `RegisterClient`'s reply is decoded to show the latest client id and to fill it into the
`RegisterSample` form.

### One dispatch path per action

No action has both a typed call and a form. Two paths to one action is two places for the behaviour to differ —
a typed capture that took a `double` would round an over-precise reading that the form submits exactly and the
model refuses (decision 7). There is no `double` anywhere on the client: readings are entered and shown as exact
decimals.

### What the tests cover

`tests/client/` drives each controller against the real models in-process (`test_sample_controller.cpp`,
`test_result_controller.cpp`), mounts each tab on a recording backend and drives it through its controls
(`test_lims_views.cpp`), and starts the whole application on each frontend the build has
(`tests/smoke/test_lims_frontends.cpp`).

**What looks different from the old Qt Quick screens.** Behaviour is kept; the layout is each frontend's own. The
return and reject forms are hidden outside the states they apply to instead of disabled; the open-existing id is a
text field instead of a spin box; the results are a table; and a capture records the `--user` who made it, where the
old client installed no principal.
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake build/all && cmake --build build/all --target ladder_lims_tests lims
./build/all/examples/lims/ladder_lims_tests
```

Then the check from Step 1. Expected: every lims case passes; the check prints "lims' QML stack is gone".

Mutation check: in `examples/lims/include/lims/dto/sample_dto.hpp`, set `RejectSample::explicitSubmit` to `false`.
Expected FAIL in "every form the client renders submits only on its button" (action type `RejectSample`). Restore
it.

- [ ] **Step 5: Commit**

```bash
git add -A examples/lims examples/crm/include/crm/gui/crm_schemas.hpp codecov.yml
git commit -m "wip(lims): retire lims' QML, bridges and presenters; README client section

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task M7: lims group verification, the ladder library's Qt link, and squash

With kanban's, ledger's and lims' domain libraries Qt-free, nothing in `ladder_<rung>_lib` needs Qt. Part 8's
`morph_add_rung` already links it `Qt6::Core` (and turns on its `AUTOMOC`) only while a rung has no `app/`
directory, and all three rungs now have one, so this task confirms the link is gone rather than changing the
function.

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Confirm the ladder library's Qt link**

```bash
grep -n -B1 -A3 'if(NOT EXISTS "${_dir}/app")' cmake/morph_add_rung.cmake
git grep -l -E '#include <Q|Q_OBJECT' -- 'examples/kanban/src/models' 'examples/kanban/src/db' 'examples/kanban/src/app' \
  'examples/kanban/include' 'examples/ledger/src/models' 'examples/ledger/src/db' 'examples/ledger/include' \
  'examples/lims/src' 'examples/lims/include'
ninja -C build/nofront -t commands ladder_lims_lib | grep -c QtCore
```

Expected: the first prints Part 8's guard around `target_link_libraries(ladder_${_rung}_lib PUBLIC Qt6::Core)` and
`AUTOMOC ON`; the second prints nothing; the third prints `0`. If the first prints nothing, Part 8's guard is
missing: stop and report it rather than editing `cmake/morph_add_rung.cmake` here. If the second prints files, name
them in the report as what still needs Qt.

- [ ] **Step 2: Strict build and the ladder suite**

```bash
cmake build/all && cmake --build build/all && ctest --test-dir build/all -L ladder --output-on-failure
```

Expected: every test passes.

- [ ] **Step 3: No frontend**

```bash
cmake --build build/nofront --target ladder_lims_tests ladder_lims_app
./build/nofront/examples/lims/ladder_lims_tests "[lims]"
```

Expected: builds; every case passes; there is no `lims` target.

- [ ] **Step 4: Toolkit-free and UI-free checks**

```bash
git grep -n -E '#include <Q|morph/qt|morph/tui|morph/qt_quick' -- examples/lims/app; test $? -eq 1 && echo ok-app
git grep -n 'morph/ui/' -- examples/lims/app/controllers; test $? -eq 1 && echo ok-controllers
git grep -n -E '#include <Q' -- examples/lims/include examples/lims/src; test $? -eq 1 && echo ok-domain
```

Expected: `ok-app`, `ok-controllers`, `ok-domain`.

Mutation check for Step 1: `git mv examples/lims/app examples/lims/app.off`, re-run `cmake build/nofront`, and the
third command of Step 1 prints a positive count (the guard links `Qt6::Core` again for a rung without `app/`). Move
it back and re-run the configure.

- [ ] **Step 5: Sanitizers** — as Task K16 Step 4 with `-DMORPH_LADDER_RUNGS=lims`, binary
  `build/clang-tsan/examples/lims/ladder_lims_tests`, filter `"[lims][client],[lims][view]"`. Expected: clean.

- [ ] **Step 6: clang-tidy over the changed lines** — CONTRIBUTING's recipe, `origin/master...HEAD`, file count
  asserted non-zero. Expected: no findings in `examples/lims/`.

- [ ] **Step 7: Commit any fixes** (`wip(lims): fixes from the sanitizer and tidy gates`), or say there were none.

- [ ] **Step 8: Squash the group into its one commit**

Follow the master plan's "Squashing a part" procedure with key `lims` and this message:

```text
examples/lims: one app, any frontend

The lims client becomes a toolkit-free app library (ladder_lims_app): a
lifecycle controller and a results controller whose shared handler
attaches to the lifecycle tab's sample through a keyed query, with every
typed-field action a runtime form built from its own schema, and two tab
views of bindings. One binary picks Qt Quick or the TUI at runtime and
acts as --user, so captures record who made them. lims still has no
server of its own.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure must show `examples/lims: one app, any frontend` directly after
`examples/ledger: one app, any frontend`.
