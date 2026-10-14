# examples/common, the generic client and the gallery: implementation plan (part 10, #895)

**Goal.** One generic client renders an application's screens, fetched from a server (`--server URL`) or linked
in-process (`--app NAME`), on the frontend `ui::selectFrontend` picks. A Qt-free `morph_examples_common` gives every
example the pieces it needs for that and for its tests. `examples/gallery` is one document that uses every node
kind, and its render smoke test passes offscreen in both modes.

**Specs.** Spec 4 §2–§4 and §8 item 1 (`2026-10-04-examples-migration-design.md`), spec 1 §5b and §8
(`2026-10-04-declarative-ui-tui-design.md`), spec 6 §4–§5 for what the client mounts. In-tree:
`docs/spec/ui/frontend.md`, `docs/spec/ui/service.md`, `docs/spec/ui/host.md`, `docs/spec/core/backend.md`.

**Starting point.** The plan of 2026-10-04 (`29ca9b1f`, `…-6-examples-foundation.md`) predates specs 5 and 6: it
built per-application view code on an injected frontend. Kept from it: the Qt-free target with optional transport
translation units, `testkit/wait.hpp` over `deadline.hpp`, and "a smoke that mounts nothing must fail". Dropped:
`AppEnvironment`'s WASM query parsing, uuids and ids, the poller, `Wiring`, `examples/tui/*`, and its
`SocketBackend` connect handlers (nothing here needs them).

## What planning found

1. **The pre-connect bind queue is already on master.** `SocketBackend` queues a private bind made before the
   socket connects (`BeforeConnect::Queue`, `include/morph/net/socket_backend.hpp`), and `QtWebSocketBackend` always
   has. Spec 4 §3's "SocketBackend gains this queue" is done; this part adds nothing for it.
2. **No transport can send a `ui-*` request.** `UiClient` and `evaluatorOver` take an `Exchange`: "send one
   envelope, call back once with the reply". `docs/spec/ui/service.md` says every morph transport does that, but
   `IBackend` has no such verb: `SocketBackend` and `QtWebSocketBackend` send only the envelopes their own verbs
   build. A remote client therefore cannot run `ui-hello` at all. This is the one library change of the part:
   - `morph::backend::IEnvelopeTransport::request(wire::Envelope, exec::IExecutor& cbExec) ->
     async::Completion<wire::Envelope>`, in its own header (`core/envelope_transport.hpp`). A separate interface,
     not an `IBackend` virtual: a backend without a wire has no peer, and `IBackend`'s many implementations and test
     doubles stay untouched (and `core/backend.hpp`, which nearly every translation unit includes, unchanged). The
     reply envelope, `ok` or `err`, resolves the completion; only a transport failure rejects it.
   - The request travels with the session it carries. The ui protocol's callers (`UiClient::open(session)`,
     `evaluatorOver(…, session, …)`) already pass the session explicitly; stamping the backend's default session
     over it would silently replace what they asked for.
   - `SocketBackend` and `QtWebSocketBackend` implement it on their control path: a fresh `callId`, filed with the
     other control calls, rejected by `cancelPending`, a drop or a close. A request made while the socket is not
     connected is held and sent at the next connect, as a private bind is, because a client opens its source right
     after making the transport.
   - `ui::exchangeOver(IEnvelopeTransport&, exec::IExecutor&) -> Exchange` in `ui/service_client.hpp` adapts it.
   Own tests, a CHANGELOG line, `docs/spec/core/backend.md` and `docs/spec/ui/service.md`.
3. **Local mode can run the same path.** `UiService::handle` answers one envelope synchronously. A local source
   builds the application's bundle in-process, answers its `Exchange` with `UiService::handle`, and opens it with
   the same `UiClient`, so local and remote mode differ only in the exchange and the bridge's backend, and local
   mode verifies the bundle exactly as remote mode does. `evaluatorOver` over the same exchange gives local mode
   `ui-evaluate` too.
4. **What the client mounts needs the unfinished hosting slices.** Spec 6 §5's app root (the app document, sign-in,
   the start screen) and §7's workspace shell are the hosting lane's (#893), and the forms engine's app shell
   (`app-menu`, `app-screens`) is the forms lane's (#892). Neither exists. The client therefore mounts through a
   **`examples::ClientShell`** in `examples/common/app/client_shell.hpp`: it loads every screen of the bundle with
   the catalog's facts, mounts the start screen (`--screen ID`, else the catalog's first screen with a document)
   through the library's `DocumentScreenHost`, replaces it on `navigate`, and shows a `ConnectError` or a refused
   mount as text. It is the seam the app root replaces; it is not a framework shell. Split out, for #893: the app
   document, sign-in through a login screen, menus, tabs and dialogs from `navigate … as`, the offline switch.
5. **The forms engine does not lower yet.** The interpreter reports `form`, `collection` and `wizard` as "needs the
   forms engine" and shows a placeholder. A render smoke that fails on any diagnostic cannot pass with them in the
   gallery, and one that tolerates that diagnostic would tolerate it forever. The gallery therefore uses every node
   kind of `ui::nodeKinds()` except those three, and its coverage test checks *gallery ∪ pending = nodeKinds()* and
   *gallery ∩ pending = ∅*: a new kind fails the test until the gallery uses it, and the three move in when the
   forms lane lands. Split out, for #892 or the part that first follows it.
6. **The WebAssembly client is not part 10.** Spec 4 §3 builds the generic client for the browser, local mode for
   bank. That needs `qt_import_qml_plugins`, the `wasm-ladder.yml` job and bank's in-memory models; it moves with
   bank (#897). This part builds the client natively only.
7. **Rung layout in `morph_add_rung`** (spec 4 §6) waits for the first rung that moves (#896); no rung uses it here.

## Design

### `morph_examples_common` (`examples/common/app/`, Qt-free)

A STATIC library, `morph::examples_common`, include root `examples/common`, so `<app/…>` and `<testkit/…>`. Its
public headers name no toolkit. Two optional translation units, compiled only when their transport is built and
linked PRIVATE: `transport_qt.cpp` (`QtWebSocketBackend`, `MORPH_BUILD_QT`) and `transport_net.cpp`
(`SocketBackend`, `MORPH_BUILD_NET` on POSIX). Every `#if` macro is defined to 0 or 1 by CMake.

- **`app/app_source.hpp`**
  - `ClientArgs`: `--server URL`, `--app NAME`, `--db PATH`, `--user NAME`, `--screen ID`, each as `--x v` or
    `--x=v`; the last one wins; anything else is left for the frontend. `readClientArgs(std::vector<std::string>&)`
    removes what it read and returns `std::expected<ClientArgs, std::string>`: a value option without its value,
    both or neither of `--server`/`--app`, are errors naming the option.
  - `LocalApp{name, bundle, models}`: what local mode needs of an application: a function that builds its
    `ui::ServedBundle` (the same function its server calls), and an optional set-up run before the first call
    (`LocalSetup{db, user}`).
  - `ClientSource : ui::AppSource`: what the shell needs beside the bundle once the source is open: `dispatcher()`,
    `catalog()` (the `ui::Catalog` from the catalog reply's facts) and `startScreen()`.
  - `RemoteAppSource(url, TransportFactory)`: `open` makes the transport (`app/transport.hpp`) on the context, a
    `Bridge` over it owned by the context's executor, a `BridgeDispatcher` with `evaluatorOver`, and runs
    `UiClient::open` over `exchangeOver(transport)`. A connect that does not answer within `connectTimeout`
    (10 s) delivers `Unreachable`. `close` drops a `ready` not yet delivered, and a reply delivered after `close`
    sends nothing further: the exchange checks a liveness flag the source owns before it touches the transport.
  - `LocalAppSource(LocalApp, LocalSetup)`: `open` builds the bundle, a `UiService` over it, a worker pool, a
    `LocalBackend`, the bridge, and opens through `UiClient` over the in-process exchange.
- **`app/transport.hpp`** — `makeTransport(ui::AppContext&, url) -> std::expected<std::unique_ptr<IBackend>,
  std::string>`: a `SocketBackend` on the context's I/O loop when there is one (the terminal frontend), else a
  `QtWebSocketBackend` (Qt Quick); an error naming what is not built otherwise.
- **`app/client_shell.hpp`** — `ClientShell` and `mountClientShell(context, backend, opened, source, options)`, as
  in finding 4. `ClientShellOptions{onDiagnostic, onMounted}` let a test or `--exit-after-mount` observe it.

### Test support (`examples/common/testkit/`)

- **`wait.hpp`** — `pumpUntil(exec::MainThreadExecutor&, pred, budget)` and the `StepExecutor` overload, and
  `awaitOn(owner, Completion<T>, budget)`, scaled by `MORPH_LADDER_DEADLINE_MS` through `deadline.hpp`. No sleeps
  outside it.
- **`screen_harness.hpp`** — `ScreenHarness(screen document text or `RegisteredScreen`, catalog, mode)`: a
  `MainThreadExecutor` owner, a runtime, a `RecordingBackend`, a session, a bridge over the rig's backend (`Local`:
  `LocalBackend` on a pool; `Simulated`: `SimulatedRemoteBackend` over a `RemoteServer`), a `BridgeDispatcher`, and
  the screen mounted through `DocumentScreenHost`. Drives by test id: `click`, `edit`, `toggle`, `choose`, `text`,
  `waitFor(pred)`; collects diagnostics. A socket mode needs `morph::net` or Qt and joins with the first app that
  needs it.
- **`render_smoke.hpp`** — header-only, needs `morph::qt_quick`: `runRenderSmoke(argc, argv, ClientSource&)` runs
  a `qt_quick::Frontend` whose shell mounts every screen the catalog lists, one after another, settles each, and
  records a refused document, a mount error, an interpreter diagnostic, and any Qt warning (a failed component
  is one). It returns `SmokeReport{opened, mounted, failures}`; a run that mounted nothing is a failure by
  construction.

### The generic client (`examples/client/`)

`main.cpp` has spec 4 §3's shape: read the client's arguments, build the source, offer the frontends built
(`qt_quick::frontendOption`, `tui::frontendOption`), `selectFrontend`, run. Both frontends mount through
`mountClientShell`. `--exit-after-mount` quits with 0 once the start screen mounted with no diagnostic and 1
otherwise, which is how the binary is tested. Two executables from the one source: `morph_client` links no
application (remote only, `--app` names none), and `morph_examples_client` links the gallery (`local_apps.cpp`
lists `gallery::localApp()`). Built when `MORPH_BUILD_EXAMPLES` and `MORPH_BUILD_QT_QUICK` or `MORPH_BUILD_TUI`.

### The gallery (`examples/gallery/`)

- `gallery_screens`: a stateless model `GalleryModel` (`Gallery_Model`) with one action, `Gallery_Facts`, that
  returns the rows the table, the key-value list and the `forEach` show, so a query crosses the bridge in both
  modes; the screen `gallery.home`, written with the C++ builders; `gallery::bundle()` and `gallery::localApp()`.
- `gallery_server` (with `MORPH_BUILD_QT`): `QtWebSocketServer` with the `UiService`, `--port`.
- Tests (`gallery_tests`): the screen validates; it uses every kind of `nodeKinds()` but the pending three; it
  mounts on the harness in `Local` and `Simulated` mode with no diagnostic, and the query's rows reach the table;
  a click, an edit and a choice change what the screen shows.
- Render smoke (`gallery_render_smoke`, with `MORPH_BUILD_QT_QUICK`, offscreen): local mode through
  `LocalAppSource`, remote mode through `RemoteAppSource` against an in-process `QtWebSocketServer`; and the
  `morph_examples_client --app gallery --exit-after-mount` binary, offscreen, as a ctest.

## Commits

1. This plan.
2. `core, net, qt, ui`: the envelope request seam and `exchangeOver`, with tests, spec text and the CHANGELOG line.
3. `examples/common`: `morph_examples_common` (sources, transport, client shell) and `wait.hpp`, with tests.
4. `examples/common`: the screen harness and the render smoke helper.
5. `examples/gallery`: model, screen, server and tests.
6. `examples/client`: the generic client, the gallery's render smoke and the client ctest.

## Verification

- GCC debug with examples, Qt, Qt Quick, `morph::net` and the TUI on: build, every new test, the touched suites
  (`morph_net_tests`, `morph_qt_tests`, `morph_tests` ui cases).
- Strict clang (`-Weverything -Werror`).
- Qt Quick render smoke offscreen (`QT_QPA_PLATFORM=offscreen`).
- Doxygen with `WARN_AS_ERROR` (the seam is public API).
- clang-tidy on changed lines, one header per translation unit, a planted finding first.
- Mutations, each confirmed to fail a named test that ran: the request seam's queue, its `cancelPending` drain and
  its session; the exchange's liveness guard; the argument reader's missing value; the shell's start screen; the
  harness's diagnostics; the smoke's "mounted nothing"; the gallery's kind coverage.
- Windows by reading: no `auto*` from `std::array` or string iterators, no Catch2 assertion directly on an
  optional or expected of a type without `operator==`, every new public header self-contained.
