# Every example on an injected frontend — design (spec 4)

Every demo application under `examples/` becomes a **toolkit-free app library** — controllers
(spec 1 §4b) and views (spec 1 §5, spec 2's forms) — plus **one binary** whose `main` is the
composition root: it picks the TUI or Qt Quick frontend at runtime (`ui::selectFrontend`), wires
the transport, and hands the frontend the application factory. The hand-written QML, the
QML-facing bridges and presenters, and the Qt client infrastructure in `examples/common` go away;
the program's last commit removes them together with the shipped QML forms renderer (spec 2 §11).

## Contents

[1 Scope](#1-scope) · [2 Shape of an app](#2-shape-of-an-app) · [3 Composition root](#3-the-composition-root) ·
[4 examples/common](#4-examplescommon) · [5 The apps](#5-the-apps) · [6 Build](#6-build-and-gating) ·
[7 Testing rules](#7-testing-rules-testingmd) · [8 Commits](#8-commits) · [9 Docs](#9-docs) ·
[10 Risks](#10-risks)

## 1. Scope

| App | Today | Screens to re-express |
|---|---|---|
| bank | 13 QML files, 7 controllers, desktop + WASM (in-memory "shadow" models) | Login, shell with sidebar, Accounts, Move money, Cards, Payees, Loans |
| pastebin | 2 QML, 2 bridges + presenter, desktop + WASM | Create form, list, detail with delete |
| bookmarks | 3 QML, 4 bridges + 3 presenters, desktop + WASM | Login form, list (archived toggle, multi-select, bulk archive), detail (archive, delete, edit, import), tags (rename, merge), shared feed |
| polls | 3 QML, bridge + presenter + forms controller, **WASM only** | Landing (open by id, create), create-poll (dynamic option list), vote grid, undo, comments, finalize, live activity feed |
| kanban | 7 QML, 2 bridges + 2 presenters, desktop + headless test client | Login, projects, members (role picker), board (swimlanes, columns, cards, **drag to move**), rules, task detail (comments, attachments) |
| ledger | 5 QML, 4 bridges + 4 presenters + report poller | Tabs: ledger (accounts, entries, store, undo), budgets, rules, statement (polled job) |
| lims | 3 QML, 2 bridges + 2 presenters | Tabs: sample lifecycle (forms + actions), results (capture, verify, conflicts) |
| forms demo | 2 QML + FormsController | App shell over the lab schemas: forms, the intake wizard, the samples collection |

Not UI, unchanged: `concepts`, `qt_tls_client`, `vetted_hmac`, `crm` (library and tests only).
**Servers stay on Qt** (`ladder_*_server`, QCoreApplication + QtWebSocketServer), and so does the
server side of the multi-mode test rig: only client code must be toolkit-free.

## 2. Shape of an app

```
examples/<app>/
  include/, src/models/, src/db/   domain library — Qt-free (unchanged except as below)
  src/server/                      server executable — Qt, as today
  app/                             app library: controllers/, views/, <app>_application.{hpp,cpp}
  ui/main.cpp                      the one binary (native and WebAssembly)
  tests/                           model tests, controller tests, view tests, frontend smoke tests
```

- **The domain library loses Qt.** Today `ladder_<rung>_lib` links `Qt6::Core` because pastebin's,
  bookmarks' and ledger's `App` classes are QObjects with QTimers (expiry sweep, metadata fetch,
  outbox relay, report runs). Those classes run only in the server, so they move into
  `src/server/` and the server target.
- **`app/`** is target `<app>_app` (bank: `bank_app`; rungs: `ladder_<rung>_app`), linking only
  `morph`, the domain library and `examples/common`'s Qt-free app layer. A toolkit include in it fails to build — the claim that the
  application is toolkit-free is enforced by the build. It holds:
  - `controllers/`: one controller per screen (spec 1 §4b's shape: handlers, Store, Queries,
    Mutations, Computed projections, `CallbackScope`s last). They replace the bridges, presenters
    and QML conditionals; formatting moves into `Computed`s (bank's `Format.hpp` becomes
    `std::string` functions).
  - `views/`: `ui::Node` builders per screen, bindings only, and `forms::formView` /
    `collectionView` for the schema-driven parts.
  - `<app>_application`: the `ui::Application` — it owns the controllers and returns the root view
    (navigation is a `Switch` over a route signal: login → list → board).
- **Timers become scheduler-driven.** `EventPoller`, `ReportJobPoller` and debounce `QTimer`s become
  `Query` `refreshEvery` or `Scheduler` calls; `QUuid` idempotency keys become a random-UUID helper
  in `examples/common`.

## 3. The composition root

`ui/main.cpp`, the same for every app:

```cpp
int main(int argc, char** argv) {
    auto env = examples::AppEnvironment::fromArgs(argc, argv);   // --server, --db, --user, --seed
    std::vector<ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
    built.push_back(qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
    built.push_back(tui::frontendOption());
#endif
    auto frontend = ui::selectFrontend(built, argc, argv);
    return frontend->run([&](ui::AppContext& ctx) { return <app>::client::makeApplication(ctx, env); });
}
```

- **`examples::Transport`** (in `examples/common/app/`) builds the `Bridge` inside the factory, on
  `ctx.executor()`:
  - Local: `LocalBackend` over a `ThreadPoolExecutor` (under WebAssembly over the owner executor,
    as today), the database set up from `--db`;
  - Remote (`--server URL`): on Qt Quick, `QtWebSocketBackend`; on the TUI, `morph::net`'s
    `SocketBackend` on `ctx.ioLoop()` — POSIX only, so a Windows TUI client runs local-only, and
    asking for `--server` there is an error naming the reason.
- Login is a screen of the app (bank, bookmarks, kanban) or a `--user` principal installed with
  `setDefaultSession` (ledger, lims, as their GUIs do today with a dev principal).
- WebAssembly: the same `main` with only Qt Quick built; polls' `?poll=` URL parameter becomes an
  `AppEnvironment` field filled by the WASM build's URL reader.

## 4. examples/common

Added first (spec 4's first commit), Qt-free, target `morph_ladder_app_common`:

- `app/app_environment.hpp`, `app/transport.hpp` (§3), `app/uuid.hpp`, `app/ids.hpp` (strong id ↔
  `std::string`, replacing `id_qml.hpp`'s QString forms), and `app/poller.hpp` — an "events since
  cursor" poller over a `Query` with `refreshEvery`, replacing `EventPoller`.
- `testkit/`: `wait.hpp` — `pumpUntil` over a `MainThreadExecutor` or `StepExecutor` with the
  `deadline.hpp` budget (the Qt `pump.hpp` stays only for the rig's Socket mode, whose server is
  Qt); `frontend_smoke.hpp` — runs an `ApplicationFactory` on the TUI (`ScriptedInputSource`) and
  on Qt Quick (offscreen) and asserts it mounts and quits.

Removed last (§8): `gui/presenter.*`, `gui/event_poller.*`, `gui/error_text.*`, `gui/id_qml.hpp`,
`gui/app_context.*` (`morph_ladder_gui`, `morph_ladder_app`), `testkit/qml_surface.*` and its
self-tests, and the QGuiApplication branch of `testkit_main.cpp`. `backend_rig`, `fault_proxy`,
`offline_rig`, `process_pool` and the Qt-free fixtures stay.

## 5. The apps

Per app: the screens of §1, each with its controller and view, and the behaviours its current
presenter/bridge tests pin re-expressed as controller tests (the plan maps each existing test file).
App-specific decisions:

- **bank** joins the rung layout (`bank/app/`, `bank/ui/`) though it stays outside `rungs.txt`. Its
  six controllers become `AuthController`, `AccountsController`, `TransactionsController`,
  `CardsController`, `PayeesController`, `LoansController`. The WebAssembly build keeps its
  in-memory models behind the same handlers. `bank/gui/`, `bank/gui_wasm/` and
  `MORPH_BUILD_BANK_GUI` are removed; the binary is `bank`. The A/B stale-selection ordering of
  spec 1 §4b gets a controller test.
- **pastebin**: create is a typed `forms::Form<CreatePaste>`; the list is a `Query<ListPastes>`;
  opening a paste is an explicit `Mutation<GetPaste>` — `GetPaste` consumes one of the paste's
  allowed reads, so it must never be refetched behind the user's back; delete invalidates the list.
- **bookmarks**: the forms (Login, CreateBookmark, EditBookmark, ImportBookmarks, RenameTag,
  MergeTags) are runtime forms over the schemas it already emits, routed by `bridgeSubmitter`
  across AuthModel, BookmarkModel and TagModel; the list is a `Table` with multiple selection and
  bulk archive/unarchive.
- **polls** gains a native binary beside its browser client. Create-poll's option list is a
  `forEach` over a signal of option drafts (2–20, add/remove); the vote grid is a `Grid` of radio
  `Select`s; the activity feed is a `Poller`.
- **kanban**: the board is a `Grid` of swimlanes × columns of cards; a card has `dragKey` = its
  task id and a column `onDrop` → the `moveTask` `Mutation` (with a fresh idempotency key), on both
  frontends. Attachments use an injected `IAttachmentTransfer` (upload a file path, download to a
  path, list): the Qt implementation is `QNetworkAccessManager` (built only with Qt), the TUI's a
  minimal HTTP/1.1 client over POSIX sockets inside `examples/kanban/app/` (so the Windows TUI
  shows attachments disabled). The offline queue and network monitor stay in the board controller.
  The headless test client keeps driving the board controller over `QtWebSocketBackend`.
- **ledger**: the statement job poll is a `Query` with `refreshEvery`; `ImportOpId`s come from the
  UUID helper.
- **lims**: its forms are runtime forms; still no server (its README's known gap).
- **forms demo**: `morph_forms_qml` becomes `morph_forms_app`, rendering `LabApp` through
  `appShellView` — forms, the intake wizard, and `SamplesView` as an app-shell `view` screen.
  `--typed` builds the same screens from `forms::Form<A>`s instead of runtime schemas, showing both
  entrances side by side. The CLI (`--schemas`, REPL, `--emit-html`) is unchanged.

## 6. Build and gating

- An app binary is built when its example is enabled and **at least one frontend** is
  (`MORPH_BUILD_TUI` or `MORPH_BUILD_QT_QUICK`); it links every frontend that was built and defines
  `MORPH_EXAMPLE_HAS_TUI` / `MORPH_EXAMPLE_HAS_QT_QUICK` accordingly. Controller and view tests
  need neither frontend.
- `morph_add_rung` learns the new layout: `app/*.cpp` → `ladder_<rung>_app`, `ui/main.cpp` →
  `<rung>` (the `gui/`, `gui_lib/`, `gui/qml/` and `gui_wasm/` conventions are removed in the last
  commit, once no rung uses them). `MORPH_BUILD_LADDER` still requires `MORPH_BUILD_QT` (servers,
  Socket rig mode); a rung's `app/` and tests do not use Qt.
- CI: the ladder and bank jobs configure `MORPH_BUILD_TUI=ON` and `MORPH_BUILD_QT_QUICK=ON`;
  `wasm-ladder.yml` and `wasm-demo.yml` build the apps' `ui/main.cpp` with Qt Quick only.

## 7. Testing rules (TESTING.md)

The "Presenter architecture" rules and the QML-surface drift guard are replaced by:

1. **The app library is toolkit-free**, enforced by its target's link set.
2. **Views are bindings only**; every conditional, format and validation lives in a controller or a
   `Computed`.
3. **Controllers are tested headless**: a `Runtime` on a `MainThreadExecutor` or `StepExecutor`,
   the multi-mode rig (Local, Simulated, Socket), waits through `testkit/wait.hpp`, never sleeps.
4. **One view test per screen** on `RecordingBackend`: the screen's tree dump, and that its
   controls drive the controller.
5. **One smoke test per frontend per app** (`frontend_smoke.hpp`): the application mounts and quits
   on the TUI and on Qt Quick offscreen.
6. Fingerprints, journeys, convergence and stress harnesses are unchanged: they drive controllers
   instead of presenters.

## 8. Commits

In order, each building and passing on its own:

1. `examples/common: Qt-free app environment, transport, poller and test waits` — additions only.
2. One commit per app — `examples/bank`, `examples/pastebin`, `examples/bookmarks`,
   `examples/polls`, `examples/kanban`, `examples/ledger`, `examples/lims`, `examples/forms` — each
   adding the app library and binary, moving its tests, deleting that app's QML, bridges,
   presenters and old mains, and updating its README.
3. `forms, examples: retire the QML renderer and the Qt client stack` — spec 2 §11 plus §4's
   removals and `morph_add_rung`'s old conventions.

## 9. Docs

`examples/TESTING.md` (§7, and its "QML-surface drift guard", "WASM reality" and build sections),
`examples/IMPLEMENTATION.md` ("GUI minimalism" becomes "One app, any frontend"),
`examples/LADDER.md` (clients are toolkit-free apps on an injected frontend), and each app's README
client section, including the visible differences from its old QML screens (bank's custom
components and kanban's board styling are not reproduced; behaviour is).

## 10. Risks

- **Size.** Thirty-eight QML files and about 12,800 lines of Qt-facing C++ are re-expressed; the
  per-app commits keep each reviewable, and each app's existing test claims are the parity bar.
- **The Windows TUI** has no remote mode and no kanban attachments, because `morph::net` is POSIX
  only; Qt Quick on Windows has both.
- **Behaviour drift during re-expression.** Mitigated by moving each app's presenter/bridge tests
  onto its controllers before deleting the old code, in the same commit.
