# Every example on server-defined screens — design (spec 4)

Every demo application under `examples/` defines its screens on its server, as UI documents
(spec 5), next to its models. One **generic client** renders any of them: it connects to a server,
fetches the application's screens and components, and renders them with Qt Quick (spec 3), natively
or in the browser. An application that runs without a server links its models and screens into the
same client (local mode). The hand-written QML, the QML-facing bridges and presenters, and the Qt
client infrastructure in `examples/common` go away; the last pull request removes them together with
the shipped QML forms renderer (spec 2 §11).

## Contents

[1 Scope](#1-scope) · [2 Shape of an app](#2-shape-of-an-app) · [3 The generic client](#3-the-generic-client) ·
[4 examples/common](#4-examplescommon) · [5 The apps](#5-the-apps) · [6 Build](#6-build-and-gating) ·
[7 Testing rules](#7-testing-rules-testingmd) · [8 Pull requests](#8-pull-requests) · [9 Docs](#9-docs) ·
[10 Risks](#10-risks)

## 1. Scope

| App | Today | Screens to define |
|---|---|---|
| bank | 13 QML files, 7 controllers, desktop + WASM (in-memory models) | Login, shell with sidebar, Accounts, Move money, Cards, Payees, Loans |
| pastebin | 2 QML, 2 bridges + presenter, desktop + WASM | Create form, list, detail with delete |
| bookmarks | 3 QML, 4 bridges + 3 presenters, desktop + WASM | Login, list (archived toggle, multi-select, bulk archive), detail (archive, delete, edit, import), tags (rename, merge), shared feed |
| polls | 3 QML, bridge + presenter + forms controller, WASM only | Landing (open by id, create), create-poll (dynamic option list), vote grid, undo, comments, finalize, live activity feed |
| kanban | 7 QML, 2 bridges + 2 presenters, desktop + headless test client | Login, projects, members (role picker), board (swimlanes, columns, cards, drag to move), rules, task detail (comments, attachments) |
| ledger | 5 QML, 4 bridges + 4 presenters + report poller | Tabs: ledger (accounts, entries, store, undo), budgets, rules, statement (polled job) |
| lims | 3 QML, 2 bridges + 2 presenters | Tabs: sample lifecycle (forms + actions), results (live recalculation, verify, conflicts); its tab strip is the workspace shell (spec 6 §7) |
| forms demo | 2 QML + FormsController | App shell over the lab schemas: forms, the intake wizard, the samples collection |

Not UI, unchanged: `concepts`, `qt_tls_client`, `vetted_hmac`, `crm`. **Servers stay on Qt**
(`QtWebSocketServer`); they gain the screens, and the framework's handshake, catalog and bundle
envelopes serve them.

## 2. Shape of an app

```text
examples/<app>/
  include/, src/models/, src/db/   domain library — Qt-free
  screens/                         screen definitions (C++ builders, spec 5 §11) and the app shell
  components/                      the app's custom QML components, if any (spec 5 §10)
  src/server/                      server executable — Qt; registers models, screens, components
  tests/                           model tests, screen tests, render smoke tests
```

- **The domain library loses Qt.** pastebin's, bookmarks' and ledger's `App` classes are QObjects
  with QTimers that only the server runs; they move into `src/server/`.
- **`screens/`** is target `<app>_screens` (`ladder_<rung>_screens` for a ladder rung, §6), linking `morph` and the domain library only. It is linked
  by the server and by the app's local-mode client. A Qt include in it fails to build.
- **Screens replace controllers.** What the bridges, presenters and QML conditionals did becomes
  declarations: queries, mutations, relations, checks and watches. Formatting is the expression
  library's display functions; anything the library cannot express is a field the model computes
  into its reply (spec 5 §1).
- **Custom components** reproduce what plain controls cannot, such as bank's account cards and
  kanban's board styling. Each has a fallback of built-in nodes.

## 3. The generic client

`examples/client/main.cpp`, built once:

```cpp
int main(int argc, char** argv) {
    auto source = examples::appSource(argc, argv);   // --server URL, or --app NAME for local mode
    std::vector<ui::FrontendOption> built;
#if MORPH_CLIENT_HAS_QT_QUICK
    built.push_back(qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_CLIENT_HAS_TUI
    built.push_back(tui::frontendOption());
#endif
    std::vector<std::string> args{argv, argv + argc};
    auto frontend = ui::selectFrontend(built, args);  // removes --ui (spec 1 §5b)
    if (!frontend) { std::cerr << frontend.error().message() << '\n'; return 2; }
    return (*frontend)->run(*source);
}
```

- **Remote** (`--server URL`): a `RemoteAppSource` connects with the transport for the chosen
  frontend — `QtWebSocketBackend` under Qt Quick, `morph::net::SocketBackend` on the I/O loop under
  the terminal — performs the handshake, and renders what the server serves. The generic client
  binary contains no example code.
- **Local** (`--app NAME`): a `LocalAppSource` runs the named application's models and screens
  in-process over `LocalBackend`, with the database set up from `--db`. A local-mode client links
  the applications it offers; the example build produces `morph_examples_client`, which links all of
  them.
- **WebAssembly:** the generic client built with Qt Quick only. Remote mode connects to any example
  server. Local mode is available for the applications whose models build under Emscripten — bank,
  with its in-memory models, is the browser demo that needs no server. kanban, ledger and lims need
  the SQLite ORM and are remote-only in the browser.
- **Sessions.** Login is a screen of the application (bank, bookmarks, kanban). ledger and lims
  sign in a development principal from `--user` in local mode and through their login screen
  remotely.
- **The transport binds early.** A handler is bound as soon as a screen declares its model, which may
  be before the socket has connected. Both transports queue a bind made before the first connect and
  send it once connected; `SocketBackend` gains this queue, which `QtWebSocketBackend` already has.

## 4. examples/common

Qt-free, target `morph_examples_common`:

- `app/app_source.hpp` — the argument reader and the remote and local sources of §3;
  `app/transport.hpp` — transport selection by frontend.
- `testkit/screen_harness.hpp` — loads a registered screen, binds it to a multi-mode rig backend
  (Local, Simulated, Socket), mounts it on `RecordingBackend`, and drives it: edit, choose, click,
  wait. Every app's screen tests use it, so no app hand-writes a client rig.
- `testkit/wait.hpp` — `pumpUntil` over a `MainThreadExecutor` or `StepExecutor` with the
  `deadline.hpp` budget.
- `testkit/render_smoke.hpp` — opens an application's every screen in the Qt Quick renderer
  offscreen and asserts each mounts without a refused document, a failed component or a QML warning.

Removed in the last pull request: `gui/presenter.*`, `gui/event_poller.*`, `gui/error_text.*`,
`gui/id_qml.hpp`, `gui/app_context.*`, `testkit/qml_surface.*` and its self-tests, and the
QGuiApplication branch of `testkit_main.cpp`. `backend_rig`, `fault_proxy`, `offline_rig`,
`process_pool` and the Qt-free fixtures stay.

## 5. The apps

Per app: the screens of §1, defined in `screens/`, and the behaviours its presenter and bridge tests
pin today re-expressed as screen tests on the harness. The plan for each app maps every existing test
file to its replacement. App-specific decisions:

- **bank:** the shell is the app shell with a sidebar menu. Account cards, the transfer flow and the
  loan schedule use custom components. The browser demo is local mode over the in-memory models.
  The A/B stale-selection ordering gets a screen test.
- **pastebin:** create is a `form`; the list is a query; opening a paste is an `exclusive` mutation of
  `GetPaste`, which consumes one of the paste's allowed reads, so it is never refetched and a second
  open while one is in flight is refused; delete invalidates the list.
- **bookmarks:** Login, CreateBookmark, EditBookmark, ImportBookmarks, RenameTag and MergeTags are
  `form` nodes over three models (AuthModel, BookmarkModel, TagModel) in one screen; the list is a
  table with multiple selection and a bulk archive mutation.
- **polls:** create-poll's option list is a `forEach` over a state list of option drafts (2–20,
  add/remove); the vote grid is one table whose columns are the options; opening a poll is a
  `latest` mutation, so only the newest open applies; a reconnect re-opens the poll and keeps the
  participant's picks, which are client state; the vote button sends `SubmitVotes` until a vote
  succeeds for the current participant name and `UpdateVotes` after; the activity feed is a query
  with `refreshOn`.
- **kanban:** the board is a grid of swimlanes by columns of cards; a card is draggable with its task
  id and a column is a drop target whose drop runs the `moveTask` mutation, `serial` per task.
  Dropping onto a card inserts before it in the target cell. Attachments use the client's file
  commands — upload a picked file, download to a chosen path — over the server's attachment side
  channel, which uses TLS whenever the WebSocket does. The offline queue and network monitor are
  the client's, enabled by the screen.
- **ledger:** the statement job is a query with `refreshEvery` that skips while a poll is in flight,
  and that stops once the job is finished or failed; each outcome has a screen test.
- **lims, sample entry:** a second reference screen, `lims.sample`, is the reference for hosting a
  record-editing dialog (spec 6) and a model-derived table (spec 7). It is implemented and unit
  tested as an example, with these parts, each chosen to exercise one feature of the program:
  - *Lifecycle (spec 6 §3):* `params: {sampleId: int?}`, `identity: sampleId`, `title` the sample
    number, `dirty` from the section edits, `onMount` loading or creating, a close guard.
  - *Private instance:* the sample model is bound without `instance`, so each open screen holds its
    own working copy, and Save commits it.
  - *Sections as forms (spec 2):* header, sampling conditions and a measurement section, each a
    `form` over its section action with a `fields` overlay (hidden member, label, unit, decimals, an
    epoch-day date).
  - *Optional measurements:* turbidity and nitrate cards that are added from a menu and removed by
    a clear mutation that `invalidates` the snapshots, over a state list.
  - *Computed on the server:* a dilution series whose rows carry measured value, recovery
    percentage, limits and an `outsideLimits` flag, all computed by the model (spec 6 §10); the card
    header counts the rows outside limits.
  - *Host components (spec 6 §6):* `SeriesGrid` and `RecoveryCurve` over that list, each with a
    `table` or `text` fallback.
  - *Errors:* save returns an enum outcome and typed errors; messages are selected by `errorKind`
    and by the enum.
  - *Lookup table (spec 7):* a "choose project" dialog is a `table` over a server-mode list action,
    with sort, a filter and single selection.
  - *Backend switch (spec 6 §9):* the screen survives a switch from a remote to a local backend
    through `onBackendChange`.
  The lims `SampleModel` gains the section, snapshot and working-copy actions this needs (spec 6
  §11 lists them); its existing actions are reused.
  The screen is authored with the C++ builders, registered with `MORPH_REGISTER_SCREEN`, validated at
  registration, and mounted by the screen harness on `RecordingBackend`. Its tests are those listed
  under spec 6 §12 for lifecycle, forms, errors and offline switching, run against this screen, and
  the render smoke test opens it on Qt Quick offscreen. No name from any application outside this
  repository appears in the example, its tests or its data.
- **lims:** the results tab is the reference for live recalculation (spec 5 §7): a row of capture
  values per result; a preview per row keyed on its draft, through `evaluate` of the capture action
  where its `computedFields` suffice; the calculated concentration shown stale while in flight; and
  a write-through capture that patches the row. Its forms are `form` nodes.
- **forms demo:** `LabApp` is the app shell — forms, the intake wizard, and `SamplesView` as a
  `collection`. `--typed` builds the same screens from `forms::Form<A>` in local mode. The CLI
  (`--schemas`, REPL, `--emit-html`) is unchanged.

## 6. Build and gating

- `<app>_screens` and the screen tests need no frontend and no Qt.
- The generic client is built when `MORPH_BUILD_QT_QUICK` or `MORPH_BUILD_TUI` is on, and links the
  renderers that are built.
- `morph_add_rung` learns the layout: `screens/*.cpp` → `ladder_<rung>_screens`,
  `components/*.qml` → the rung's component bundle installed beside its server. `MORPH_BUILD_LADDER`
  still requires `MORPH_BUILD_QT` for the servers.
- CI: the ladder and bank jobs build the generic client with Qt Quick and run the render smoke tests
  offscreen; `wasm-ladder.yml` builds the WebAssembly client and watches `src/qt_quick/**`;
  `wasm-demo.yml` builds the bank browser demo in local mode. Test executables that link Qt pass
  `DL_PATHS` on Windows.

## 7. Testing rules (TESTING.md)

The "Presenter architecture" rules and the QML-surface drift guard are replaced by:

1. **Screens are toolkit-free**, enforced by their target's link set.
2. **Every registered screen validates** (spec 5 §3) in the server's own tests.
3. **Screen tests run headless** on the harness: a document on `RecordingBackend`, the multi-mode
   rig, waits through `testkit/wait.hpp`, never sleeps.
4. **One render smoke test per app** on Qt Quick offscreen, through `render_smoke.hpp`.
5. Fingerprints, journeys, convergence and stress harnesses are unchanged; they drive screens through
   the harness, each simulated client on its own rig bridge, and compare what each client shows.

## 8. Pull requests

Examples move a few per pull request, each building and passing on its own:

1. `examples/common` and the generic client, with the gallery.
2. lims and the forms demo: the live-recalculation reference, the sample-entry reference screen
   and the app shell.
3. bank.
4. pastebin, bookmarks, polls.
5. kanban, ledger.
6. The retirement: spec 2 §11, §4's removals, and `morph_add_rung`'s old conventions.

## 9. Docs

`examples/TESTING.md` (§7), `examples/IMPLEMENTATION.md` ("GUI minimalism" becomes "Screens on the
server"), `examples/LADDER.md` (clients are server-defined screens in a generic client), each app's
README client section, and `docs/GETTING-STARTED.md`'s client walkthrough.

## 10. Risks

- **Size.** Thirty-eight QML files and about 15,900 lines of Qt-facing C++ are re-expressed; per-app
  pull requests keep each reviewable, and each app's existing test claims are the parity bar.
- **The expression library may not cover a screen.** A gap is closed by a server-computed field or a
  custom component, and a gap that two apps share is an expression-library or vocabulary addition.
- **Behaviour drift during re-expression.** Each app's presenter and bridge tests move onto its
  screens before the old code is deleted, in the same pull request.
