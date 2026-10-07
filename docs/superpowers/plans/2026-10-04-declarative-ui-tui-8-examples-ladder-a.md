# Declarative UI, Part 8 — the ladder rungs pastebin, bookmarks and polls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Re-express pastebin, bookmarks and polls as toolkit-free app libraries (controllers, views, an
`ui::Application`) plus one `ui/main.cpp` binary each that picks the TUI or Qt Quick at runtime, and delete their
QML, `gui_lib`, `gui`, `gui_wasm` and the tests that only covered those.

**Architecture:** Each rung gains `app/` (target `ladder_<rung>_app`: `controllers/`, `views/`,
`<rung>_application.{hpp,cpp}`) and `ui/main.cpp` (target `<rung>`), on the `morph_add_rung` conventions Part 6
adds. Controllers own `BridgeHandler`s, `Query`/`Mutation` nodes, forms-engine sessions and `Computed` display
projections; views are `ui::Node` bindings over them; the application owns the transport (`examples::connect`)
and the controllers. The domain library loses Qt: pastebin's and bookmarks' `App` QObjects move to a
server-only `ladder_<rung>_server_app` library this plan adds to `morph_add_rung`.

**Tech Stack:** C++23; `morph::reactive` (Part 1), `morph::ui` (Part 2), `morph::tui` (Part 3),
`morph::qt_quick` (Part 4), the forms engine (Part 5: `Form<A, M, S>`, `FormSession`, `formView`,
`handlerChoiceFetcher`), `examples/common/app` (Part 6: `connect`, `Connection`, `Wiring`, `mapCompletion`,
`Poller`) and its testkit (`frontend_smoke.hpp`); Catch2 v3; the ladder testkit's `BackendRig`, `DbFixture` and
`pumpUntil`.

**Spec:** `docs/superpowers/specs/2026-10-04-examples-migration-design.md` (spec 4) §1–§3, §5 (pastebin,
bookmarks, polls), §6, §7, §8, §9.

This is **Part 8 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` commit convention every task below follows. This part has **three groups** with keys `pastebin`
(Tasks 1–8), `bookmarks` (Tasks 9–17) and `polls` (Tasks 18–25); each group ends by squashing its own `wip`
commits into one commit, in that order.

## Global Constraints

- Part 1's constraints apply unchanged (SPDX first line, `#pragma once`, naming, comments without history,
  `-Weverything -Werror`, clang-tidy clean, `Signed-off-by` trailer).
- Examples only: **no `CHANGELOG.md` entry, no `docs/spec/` file** (CONTRIBUTING, "The changelog"). Each rung's
  README carries the record; `docs/GETTING-STARTED.md`'s pastebin walkthrough is kept true by the pastebin group.
- `app/` is toolkit-free: no `<Q…>`, `morph/qt*`, `morph/tui`, `morph/qt_quick` include anywhere under `app/`,
  and `ladder_<rung>_lib` links no Qt once the rung has `app/`. Each group's verification task proves both from
  the build's own command lines, not from reading CMake.
- Controllers include nothing from `morph::ui`; views contain bindings only — every format and every
  `std::string` a view shows is produced by a controller (`Computed` or a plain accessor).
- Identifiers obey `readability-identifier-length` (≥ 3 characters for parameters and variables, lambda
  parameters and test locals included; the only short names allowed are parameters `i j k x y n N fn cb op` and
  variables `i j k x y lk cb op fn`): never `id`, `c`, `rt`, `ui` as a parameter or variable — `pasteId`,
  `controller`, `runtime`. Struct and class data members are exempt, but this plan's fixtures name theirs
  `runtime` too. The namespace alias `namespace ui = ::morph::ui;` is allowed (it is not a variable).
- Headers under `app/` use no unchecked `operator[]` on a container
  (`cppcoreguidelines-pro-bounds-avoid-unchecked-container-access`): `.at()` or iteration.
- Each rung exposes `[[nodiscard]] std::unique_ptr<ui::Application> <rung>::client::makeApplication(ui::AppContext&
  ctx, examples::AppEnvironment const& env)` (the interface contract's "Conventions for every example
  application"); its local database setup and any other knob are built inside from `env`, so every `ui/main.cpp`
  has the shape of spec 4 §3.
- Every existing **model test file stays byte-identical** (each group's verification diffs them against
  `master`). Only `test_*_presenter.cpp`, `test_*_qml_bridges.cpp` and `test_gui_qml_smoke.cpp` are deleted.
- Controller and view tests run over the ladder's `BackendRig` rather than Part 6's `FakeAppContext`: the three
  backend modes it offers (Local, LocalSingleThread, Socket) are what the deleted presenter suites covered. Its
  client executors are Qt-driven (`QtExecutor`, `QtDrivenMainThreadExecutor`), so the wait is
  `testkit/pump.hpp`'s `pumpUntil` (Part 6 keeps that header for exactly this case). Test code never names a Qt
  type; the reactive `Runtime` is built on `*rig.executor()`.
- Frontend smoke tests live in `tests/smoke/` (their own binary, C3); every other client test in `tests/`.
- Client namespaces are `pastebin::client`, `bookmarks::client`, `polls::client` (`pastebin::app` and
  `bookmarks::app` stay the servers' `App`).

## What this plan consumes from Parts 2, 5 and 6

The interface contract fixes names; these are the further facts this plan relies on, as Parts 2, 5 and 6 define
them. The **first step of Task 1** confirms each against the tree; one that does not hold means an upstream part
did not land as its plan says — stop and report it rather than adapting here.

- **C1 (Part 6 Task 9, `cmake/morph_add_rung.cmake`):** `app/**/*.cpp` → STATIC `ladder_<rung>_app` (alias
  `morph::ladder_<rung>_app`), PUBLIC include dirs `${_dir}/app` and `${_dir}/include`, linking `morph::morph`,
  `morph::ladder_app_common` and, natively, `morph::ladder_<rung>_lib` (Lightweight's includes demoted to SYSTEM);
  `apply_warnings` on it. It is built under Emscripten too, without the rung library.
- **C2 (Part 6 Task 9):** `ui/*.cpp` → executable `<rung>` (e.g. `pastebin`) through `morph_add_example_ui`,
  built when `MORPH_BUILD_TUI` (natively) or `MORPH_BUILD_QT_QUICK` is on, linking `morph::ladder_<rung>_app`, the
  rung library whole-archive and every built frontend (`morph_example_frontends`), with `MORPH_EXAMPLE_HAS_TUI`
  and `MORPH_EXAMPLE_HAS_QT_QUICK` **always defined, to `0` or `1`** (so `#if` is `-Wundef`-clean); under
  Emscripten it builds with Qt Quick only and needs `MORPH_CLIENT_ONLY`.
- **C3 (Part 6 Task 9):** `ladder_<rung>_tests` links `morph::ladder_<rung>_app` and `morph::example_testkit` and
  no longer compiles `tests/smoke/`. `tests/smoke/*.cpp` is its own binary, `ladder_<rung>_smoke_tests`, whose
  `main` is `morph_test_main` (it owns no Qt application object; the Qt Quick frontend constructs its own),
  linking the app, `morph::example_testkit` (which carries both `MORPH_EXAMPLE_HAS_*` definitions PUBLIC) and the
  whole rung library; its ctest names are prefixed `<rung>.smoke.` and labelled `ladder-<rung>`.
- **C4 (Part 6 Tasks 2–8, contract):** `examples/common/app/app_environment.hpp` (`AppEnvironment{server, db,
  user, seed, pollId}`, `fromArgs`), `transport.hpp` (`connect(ui::AppContext&, AppEnvironment const&,
  LocalSetup)`, `Connection` with `bridge()`, `callbacks()`, `ready()`, `link()`, `LocalSetup{setupDatabase,
  workers}`, `TransportError`), `uuid.hpp` (`newUuid`), `poller.hpp`, `wiring.hpp` (`Wiring{runtime, scheduler,
  bridge, callbacks}`, all borrowed), `completion_map.hpp` (`mapCompletion<To>(owner, token, from, onValue,
  onError)`), and `examples/common/testkit/frontend_smoke.hpp` (`runFrontendSmoke`, `SmokeFrontend`, which skips a
  frontend the build lacks), all reachable with `examples/common` as an include root (`#include
  "app/transport.hpp"`). `connect()` with no `--server` calls `local.setupDatabase(env.db)` when it is set, then
  builds a `LocalBackend`. `Connection::ready()` is a tracked `reactive::Signal<bool> const&`: always true for a
  local link, the transport's state for a remote one. `AppEnvironment::fromArgs` also reads the page url's
  `?server=` and `?poll=` under WebAssembly (`detail::applyQuery`).
- **C5 (Part 6 Task 7, `poller.hpp`):** `examples::Poller<Event, Cursor>(reactive::Runtime&, reactive::Scheduler&,
  Fetch, Cursor start, OnEvent, PollerOptions = {})` with `Page` = `PollPage<Event, Cursor>{events, next}`,
  `Fetch = std::function<async::Completion<Page>(Cursor const&)>`, `OnEvent = std::function<void(Event const&)>`,
  `stoppedBy()` and `PollerOptions{interval}`. A poller starts from a fixed cursor, so the vote controller holds
  one per open poll; every use sits in `VoteController::{startFeed, stopFeed, eventsSince}` and its
  `_onFeedStopped` effect (Task 21).
- **C6 (Part 2 Task 2):** `RecordingBackend` names a widget kind by its widget interface without `Widget`
  (`"Button"`, `"Text"`, `"Table"`, `"TextInput"`, `"Select"`, `"Panel"`, `"Checkbox"`), except that a stack is
  `"Column"` or `"Row"` by its axis (a table row is a `"Row"`), and a prop by its setter without `set`, first
  letter lower-cased (`label`, `text`, `enabled`, `visible`, `title`, `placeholder`, `selected`), booleans as
  `true`/`false`. The mount sets `visible`/`enabled` only when bound or `false`, so a bound `enabled` reads
  `"true"` or `"false"` and an unbound one reads empty. A hidden or disabled widget ignores the interaction
  helpers. This plan's view tests use `find`, `all`, `prop`, `exists` and the helpers; none compares a `dump()`
  or `log()`.
- **C7 (Part 5 Tasks 6, 10b, 11, 12):** `FormSession(Runtime&, FormModel, Submitter, ChoiceFetcher,
  FormSessionOptions = {})`; its `pending()`, `lastReply()` and `lastError()` are a `Mutation`'s in-flight count,
  last result and error, so they settle in one batch per reply, `lastError()` is cleared by the next success, and
  `lastReply()` is equality-gated (two identical replies are one change — which is why Task 2's `FormSuccess`
  exists: the session has no per-submission success signal). `submit()` sets `pending()` before it returns and
  issues nothing while `body()` is `nullopt`; a required text field with an empty draft does not encode;
  `prefill(json)` replaces every draft and never submits; `reset()` blanks every draft; `body()` is tracked.
  `Form<A, M, S>(Runtime&, BridgeHandler<M, S>&, ChoiceFetcher, FormSessionOptions = {})` submits through its
  session and executes on **that** handler (so a typed form on an `AllowShared` handler reaches the attached
  instance); its action's `validate()` joins readiness through `FormSessionOptions::accepts`, so a body
  `validate()` refuses is never sent — `form.ready()` and `form.session().ready()` are both false. A runtime form
  over `bridgeSubmitter` has no such gate: `validate()` runs in the handler and the refusal is `lastError()`.
  `FormModel::forAction<A>()` reads `A`'s own schema (throwing `std::logic_error` when it cannot).
  `handlerChoiceFetcher(callbacks, handlers...)` fetches Choice options through the given handlers;
  `bridgeChoiceFetcher(bridge, callbacks)` through handlers of its own. `formView(FormSession&,
  FormViewOptions{overrides, gridColumns, submitLabel, flatGridColumns})` draws the explicit-mode Submit button
  labelled `submitLabel`, its `enabled` bound to `ready()`.

---

## Review Focus

1. **Opening a paste consumes a read, so nothing but an explicit activation may issue `GetPaste`** — not a list
   refresh, not a re-render, not moving the table's selection (Task 5 test "pasteScreen: moving the selection
   opens nothing, activating a row opens it once").
2. **A controller destroyed while its form submission is in flight** must drop the reply instead of writing
   into freed storage (Task 4 test "a controller destroyed with a create in flight drops its reply"; ASan is
   the observer).
3. **A Login reply never carries the bearer token into anything a view can show**, and the session is installed
   before the reply becomes visible (Task 10 test "A Login reply is redacted, and the session is installed
   before it shows").
4. **The option-draft list at its bounds** (2 and 20) refuses removal and addition, and removing a middle draft
   keeps every other draft's key, so their rows keep their widgets (Task 18 test "option drafts: the bounds
   hold and a removal keeps the other drafts' keys").
5. **A late `OpenPoll` reply for a poll the user already left** must not attach it (Task 19 test "a reply for a
   poll the user has left never attaches it").

---

## File Structure

| File | Responsibility |
|---|---|
| `cmake/morph_add_rung.cmake` | Qt-free `ladder_<rung>_lib` once a rung has `app/`; `src/server/app/*.cpp` + `src/server/include/` → `ladder_<rung>_server_app` (Qt), linked by the server and the tests |
| `examples/common/app/form_success.hpp` | `examples::FormSuccess` — runs a callback each time a form submission settles without error |
| `examples/common/app/tests/test_form_success.cpp` | its tests, in Part 6's `examples_common_app_tests` |
| `examples/pastebin/src/server/{app,include/pastebin/app}/` | `pastebin::app::App`, moved from `src/app/` and `include/` |
| `examples/pastebin/app/controllers/paste_controller.{hpp,cpp}` | list `Query`, open/delete `Mutation`s, the `Form<CreatePaste>`, display projections |
| `examples/pastebin/app/views/paste_view.{hpp,cpp}` | `pasteScreen()` |
| `examples/pastebin/app/pastebin_application.{hpp,cpp}` | `PastebinApplication`, `makeApplication()` and the local database setup |
| `examples/pastebin/ui/main.cpp` | composition root, native and WebAssembly |
| `examples/pastebin/tests/{client_fixture.hpp,test_paste_controller.cpp,test_paste_view.cpp}`, `tests/smoke/test_pastebin_frontends.cpp` | controller, view and smoke tests |
| `examples/bookmarks/src/server/{app,include/bookmarks/app}/` | `bookmarks::app::App` and `metadata_fetcher.hpp`, moved |
| `examples/bookmarks/app/controllers/{bookmark_text,forms_routing,session_controller,shared_feed_controller,tag_controller,bookmark_list_controller,bookmark_detail_controller}.{hpp,cpp}` | display text, the six forms' routing, and one controller per screen |
| `examples/bookmarks/app/views/bookmark_views.{hpp,cpp}` | login, list, detail, tags, feed panes and the library screen |
| `examples/bookmarks/app/bookmarks_application.{hpp,cpp}`, `ui/main.cpp` | application and composition root |
| `examples/bookmarks/tests/{client_fixture.hpp,test_bookmark_text.cpp,test_forms_routing.cpp,test_session_controller.cpp,test_tag_and_feed_controllers.cpp,test_bookmark_list_controller.cpp,test_bookmark_detail_controller.cpp,test_bookmark_views.cpp}`, `tests/smoke/test_bookmarks_frontends.cpp` | tests |
| `examples/polls/app/controllers/{poll_text,landing_controller,create_poll_controller,vote_controller,polls_navigator}.{hpp,cpp}` | display text, one controller per screen, navigation |
| `examples/polls/app/views/poll_views.{hpp,cpp}` | landing, create-poll and vote screens |
| `examples/polls/app/polls_application.{hpp,cpp}`, `ui/main.cpp` | application (create offered natively, hidden in the browser) and composition root |
| `examples/polls/tests/{client_fixture.hpp,test_create_poll_controller.cpp,test_vote_controller.cpp,test_vote_forms.cpp,test_vote_activity.cpp,test_polls_navigator.cpp,test_poll_views.cpp}`, `tests/smoke/test_polls_frontends.cpp` | tests |
| `examples/{pastebin,bookmarks,polls}/CMakeLists.txt` | the WebAssembly server url on target `<rung>`; WASM-only DTO sources on `ladder_<rung>_app` |
| `.github/workflows/wasm-ladder.yml` | builds target `<rung>` for a rung with `ui/main.cpp` (Part 4 already configures `MORPH_BUILD_QT_QUICK` there) |
| `.github/workflows/ci.yml` | `ladder-tests` configures `MORPH_BUILD_TUI` (Part 4 already adds `MORPH_BUILD_QT_QUICK` to it; Parts 3 and 4 add both to `linux-all-features`) |
| `scripts/coverage.sh`, `codecov.yml` | `app/` measured, `ui/` ignored like the old `main()` shells |
| `examples/{pastebin,bookmarks,polls}/README.md`, `docs/GETTING-STARTED.md` | running it, the client, visible differences |

Deleted per group: `examples/<rung>/gui/` (with `gui/qml/`), `gui_lib/`, `gui_wasm/`,
`tests/test_*_presenter.cpp`, `tests/test_*_qml_bridges.cpp`, `tests/test_gui_qml_smoke.cpp`.

## Build and test commands (used by every task)

```bash
cmake -S . -B build/all -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_BANK_EXAMPLE=ON \
      -DMORPH_BUILD_NET=ON -DMORPH_BUILD_FORMS_QML=ON          # once (master plan, "build/all")
cmake --build build/all --target ladder_pastebin_tests         # or ladder_bookmarks_tests / ladder_polls_tests
QT_QPA_PLATFORM=offscreen ./build/all/examples/pastebin/ladder_pastebin_tests "[pastebin][controller]"
ctest --test-dir build/all -L ladder-pastebin --output-on-failure   # the whole rung
```

Run test binaries from the repository root so `DbFixture`'s `morph_ladder_test.db` lands in one place.
Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING).

---
## Group 1 — pastebin (key `pastebin`)

### Task 1: Preflight, a server-only `App` library, and pastebin's `App` moved into it

**Files:**
- Modify: `cmake/morph_add_rung.cmake` — the header's directory table; the `ladder_${_rung}_lib` block (its
  `target_link_libraries`/`set_target_properties` lines); a new `ladder_${_rung}_server_app` block directly
  before `# ── ladder_<rung>_server`; the `ladder_${_rung}_server` block; the tests block, after the
  `WHOLE_ARCHIVE` link
- Move: `examples/pastebin/include/pastebin/app/app.hpp` →
  `examples/pastebin/src/server/include/pastebin/app/app.hpp`
- Move: `examples/pastebin/src/app/app.cpp` → `examples/pastebin/src/server/app/app.cpp`
- Test: the rung's existing `tests/test_paste_model.cpp` (its expiry-sweep cases construct `pastebin::app::App`)

**Interfaces:**
- Consumes: C1–C7 (confirmed in Step 1); `morph_add_rung()`'s existing blocks headed
  `# ── ladder_<rung>_lib`, `# ── ladder_<rung>_server` and `# ── ladder_<rung>_tests` (Part 6 adds its `app/`,
  `ui/` and smoke blocks around them and edits none of the lines replaced below).
- Produces: the convention `src/server/app/*.cpp` + `src/server/include/` → STATIC `ladder_<rung>_server_app`
  (alias `morph::ladder_<rung>_server_app`, Qt6::Core, AUTOMOC), linked by `ladder_<rung>_server` and
  `ladder_<rung>_tests`; `ladder_<rung>_lib` links no Qt once `examples/<rung>/app/` exists. Both rules are
  written for any rung, by directory alone: Part 9's kanban (whose server app adds `Qt6::Network` itself) and
  ledger use them unchanged.

- [ ] **Step 1: Confirm what this plan consumes from Parts 2, 5 and 6**

```bash
grep -n 'ladder_${_rung}_app\|morph_add_example_ui\|_smoke_tests' cmake/morph_add_rung.cmake
grep -n 'MORPH_EXAMPLE_HAS_TUI\|MORPH_EXAMPLE_HAS_QT_QUICK' cmake/morph_example_app.cmake
ls examples/common/app/{app_environment,transport,uuid,poller,wiring,completion_map}.hpp \
   examples/common/testkit/{frontend_smoke,wait}.hpp
grep -n 'struct PollPage\|struct PollerOptions\|stoppedBy()' examples/common/app/poller.hpp
grep -n 'ready() const\|struct LocalSetup' examples/common/app/transport.hpp
grep -n 'struct Wiring' examples/common/app/wiring.hpp
grep -n 'mapCompletion' examples/common/app/completion_map.hpp
grep -n 'forAction()' include/morph/forms/engine/field_model.hpp
grep -n 'accepts' include/morph/forms/engine/form_session.hpp
grep -n 'handlerChoiceFetcher' include/morph/forms/engine/handler_submitter.hpp
grep -n 'submitLabel' include/morph/forms/engine/form_view.hpp
grep -n '"Column" : "Row"' include/morph/ui/testing/recording_backend.hpp
```

Expected: every command prints at least one line, matching C1–C7. These are the upstream plans' own names, already
written into the code below; a command that prints nothing means that part has not landed as its plan says — stop
and report which, rather than adapting this plan around it.

- [ ] **Step 2: Run the rung's model tests as the baseline**

```bash
cmake --build build/all --target ladder_pastebin_tests
QT_QPA_PLATFORM=offscreen ./build/all/examples/pastebin/ladder_pastebin_tests "[pastebin][model]"
```

Expected: PASS. This is the check the move below must keep green; the move adds no behaviour of its own.

- [ ] **Step 3: Teach `morph_add_rung` the server-only library and the Qt-free domain library**

In `cmake/morph_add_rung.cmake`, in the header's "Directory -> target convention" table, after the
`src/server/*.cpp` line, add:

```cmake
#   src/server/app/*.cpp + src/server/include/      -> ladder_<rung>_server_app STATIC (Qt6::Core; the server's App)
#
# A rung with app/ (the declarative client layout) gets a ladder_<rung>_lib that links no Qt: the client
# library links it, and Qt must not reach the client through it. A server's QObject App lives in
# ladder_<rung>_server_app instead, which the server and the rung's tests link.
```

In the `ladder_${_rung}_lib` block, replace

```cmake
            target_link_libraries(ladder_${_rung}_lib PUBLIC morph::morph Lightweight::Lightweight Qt6::Core)
            target_compile_features(ladder_${_rung}_lib PUBLIC cxx_std_23)
            set_target_properties(ladder_${_rung}_lib PROPERTIES AUTOMOC ON)
```

with

```cmake
            target_link_libraries(ladder_${_rung}_lib PUBLIC morph::morph Lightweight::Lightweight)
            # A rung still on the QML layout declares its QObject App in src/app/, inside this library.
            if(NOT EXISTS "${_dir}/app")
                target_link_libraries(ladder_${_rung}_lib PUBLIC Qt6::Core)
                set_target_properties(ladder_${_rung}_lib PROPERTIES AUTOMOC ON)
            endif()
            target_compile_features(ladder_${_rung}_lib PUBLIC cxx_std_23)
```

Directly before the line `# ── ladder_<rung>_server: standalone server binary (native only) ────`, insert:

```cmake
    # ── ladder_<rung>_server_app: the server's Qt-side bootstrap (native only) ──
    # A server's App may be a QObject driven by QTimers (an expiry sweep, a metadata worker, an outbox
    # relay). It runs only in the server process, so it lives here rather than in ladder_<rung>_lib. The
    # rung's tests link it as well: they drive those background jobs directly. Its headers stay under
    # src/server/include/<rung>/app/, so every #include "<rung>/app/app.hpp" keeps its spelling.
    set(_server_app "")
    if(NOT EMSCRIPTEN AND TARGET ladder_${_rung}_lib)
        file(GLOB_RECURSE _server_app_sources CONFIGURE_DEPENDS "${_dir}/src/server/app/*.cpp")
        if(_server_app_sources)
            # Listed so AUTOMOC sees a Q_OBJECT header that does not sit beside its .cpp.
            file(GLOB_RECURSE _server_app_headers CONFIGURE_DEPENDS "${_dir}/src/server/include/*.hpp")
            add_library(ladder_${_rung}_server_app STATIC ${_server_app_sources} ${_server_app_headers})
            add_library(morph::ladder_${_rung}_server_app ALIAS ladder_${_rung}_server_app)
            target_include_directories(ladder_${_rung}_server_app PUBLIC "${_dir}/src/server/include")
            target_link_libraries(ladder_${_rung}_server_app PUBLIC
                morph::ladder_${_rung}_lib morph::qt morph_qt_impl Qt6::Core)
            target_compile_features(ladder_${_rung}_server_app PUBLIC cxx_std_23)
            set_target_properties(ladder_${_rung}_server_app PROPERTIES AUTOMOC ON)
            apply_bigobj(ladder_${_rung}_server_app)
            # Lightweight's headers are not -Werror clean (see ladder_<rung>_lib) — no apply_warnings().
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_server_app)
            endif()
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(ladder_${_rung}_server_app ${AF_SANITIZER})
            endif()
            set(_server_app ladder_${_rung}_server_app)
        endif()
    endif()

```

In the `ladder_${_rung}_server` block, replace

```cmake
        file(GLOB_RECURSE _server_sources CONFIGURE_DEPENDS "${_dir}/src/server/*.cpp")
        if(_server_sources AND TARGET ladder_${_rung}_lib)
            add_executable(ladder_${_rung}_server ${_server_sources})
            target_link_libraries(ladder_${_rung}_server PRIVATE
                morph::ladder_${_rung}_lib morph::qt morph_qt_impl Qt6::Core)
```

with

```cmake
        file(GLOB_RECURSE _server_sources CONFIGURE_DEPENDS "${_dir}/src/server/*.cpp")
        # src/server/app/ is ladder_<rung>_server_app's, linked below rather than compiled twice.
        list(FILTER _server_sources EXCLUDE REGEX "/src/server/app/")
        if(_server_sources AND TARGET ladder_${_rung}_lib)
            add_executable(ladder_${_rung}_server ${_server_sources})
            target_link_libraries(ladder_${_rung}_server PRIVATE
                morph::ladder_${_rung}_lib ${_server_app} morph::qt morph_qt_impl Qt6::Core)
```

In the tests block, directly after the `unset(_lightweight_includes)` that closes the
`if(TARGET ladder_${_rung}_lib)` branch (still inside that branch), add:

```cmake
                if(_server_app)
                    target_link_libraries(ladder_${_rung}_tests PRIVATE ${_server_app})
                endif()
```

- [ ] **Step 4: Move pastebin's `App` into the server**

```bash
mkdir -p examples/pastebin/src/server/include/pastebin/app examples/pastebin/src/server/app
git mv examples/pastebin/include/pastebin/app/app.hpp examples/pastebin/src/server/include/pastebin/app/app.hpp
git mv examples/pastebin/src/app/app.cpp examples/pastebin/src/server/app/app.cpp
```

In the moved `app.hpp`, replace the sentence

```cpp
/// expiry sweep. Nothing here decides deployment mode (`Local`/`Remote`) —
/// that stays `examples/common/gui::AppContext`'s job on the client side;
/// this is exclusively the server side.
```

with

```cpp
/// expiry sweep. Nothing here decides deployment mode (`Local`/`Remote`) —
/// that is the client's (`examples::connect`, in `examples/common/app/`);
/// this is exclusively the server side, built into `ladder_pastebin_server_app`.
```

Nothing else in either file changes: `#include "pastebin/app/app.hpp"` resolves through the new library's include
directory, and `clock.hpp` through `ladder_pastebin_lib`'s.

- [ ] **Step 5: Run the tests to verify they still pass**

```bash
cmake --build build/all --target ladder_pastebin_tests ladder_pastebin_server
QT_QPA_PLATFORM=offscreen ./build/all/examples/pastebin/ladder_pastebin_tests "[pastebin][model]"
```

Expected: PASS, the same case count as Step 2. Mutation check: delete the three lines added to the tests block
(`if(_server_app) … endif()`), rebuild. Expected FAIL at link: `undefined reference to
`pastebin::app::App::App(` (the sweep cases in `test_paste_model.cpp`). Restore.

- [ ] **Step 6: Commit**

```bash
git add cmake/morph_add_rung.cmake examples/pastebin/src examples/pastebin/include
git commit -m "wip(pastebin): a server-only App library, and pastebin's App moved into it

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `examples::FormSuccess`

A form's submission is not a `Mutation` the controller owns, so it cannot carry `MutationOptions::invalidates`.
All three rungs need "after this form succeeds, refetch that and reset the form"; this is the one helper. Part 5's
`FormSession` has no equivalent (C7): it exposes `pending()`, `lastReply()` and `lastError()`, but no success
hook, and `lastReply()` is equality-gated, so a watch on it merges two identical replies into one success.

**Files:**
- Create: `examples/common/app/form_success.hpp`
- Modify: `examples/common/app/tests/CMakeLists.txt` — append `test_form_success.cpp` as the last source of
  `add_executable(examples_common_app_tests …)`
- Test: `examples/common/app/tests/test_form_success.cpp`

**Interfaces:**
- Consumes: `forms::FormSession(reactive::Runtime&, FormModel, Submitter, ChoiceFetcher, FormSessionOptions = {})`,
  `pending()`, `lastError()`, `prefill()`, `submit()`; `forms::FormModel::fromSchema`; `forms::schemaJson<A>()`;
  `reactive::Effect`, `Runtime::untracked`; `morph::ladder::testkit::StepExecutor`
  (`examples/common/testkit/step_executor.hpp`, the executor Part 6's `examples_common_app_tests` already pumps).
- Produces: `morph::examples::FormSuccess(reactive::Runtime&, forms::FormSession&, std::function<void()>)` —
  non-copyable, non-movable; header-only in `examples/common/app/`, so `morph_ladder_app_common` carries it.

- [ ] **Step 1: Write the failing test**

Create `examples/common/app/tests/test_form_success.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/runtime.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/form_success.hpp"
#include "testkit/step_executor.hpp"

namespace {

using morph::ladder::testkit::StepExecutor;
using Reply = morph::async::Completion<std::string>;

/// @brief A one-field action whose schema the session reads; nothing executes it.
struct Note {
    std::string text;
    static constexpr bool explicitSubmit = true;
};

/// @brief A submitter whose replies the test settles by hand.
struct ManualServer {
    StepExecutor* owner = nullptr;
    std::vector<Reply::Promise> replies;

    [[nodiscard]] morph::forms::Submitter submitter() {
        return [this](std::string_view, std::string) {
            auto [completion, promise] = Reply::makeSettleable(owner);
            replies.push_back(std::move(promise));
            return std::move(completion);
        };
    }
};

}  // namespace

TEST_CASE("FormSuccess runs once per successful submission and never for a failure", "[form-success]") {
    StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    ManualServer server{.owner = &owner, .replies = {}};
    auto model = morph::forms::FormModel::fromSchema("Note", morph::forms::schemaJson<Note>());
    REQUIRE(model.has_value());
    // Note has no Choice field, so the session needs no fetcher.
    morph::forms::FormSession session{runtime, std::move(*model), server.submitter(), morph::forms::ChoiceFetcher{}};
    int successes = 0;
    morph::examples::FormSuccess const watch{runtime, session, [&] { ++successes; }};
    owner.runAll();
    CHECK(successes == 0);

    session.prefill(R"({"text":"one"})");
    session.submit();
    owner.runAll();
    REQUIRE(server.replies.size() == 1);
    server.replies[0].resolve("{}");
    owner.runAll();
    CHECK(successes == 1);

    // An identical reply text is still a second success.
    session.submit();
    owner.runAll();
    REQUIRE(server.replies.size() == 2);
    server.replies[1].resolve("{}");
    owner.runAll();
    CHECK(successes == 2);

    session.submit();
    owner.runAll();
    REQUIRE(server.replies.size() == 3);
    server.replies[2].reject(std::make_exception_ptr(std::runtime_error{"refused"}));
    owner.runAll();
    CHECK(successes == 2);
}
```

In `examples/common/app/tests/CMakeLists.txt`, append `test_form_success.cpp` as the last source of
`add_executable(examples_common_app_tests …)`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target examples_common_app_tests`
Expected: FAIL — `'app/form_success.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/common/app/form_success.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <utility>

namespace morph::examples {

/// @brief Runs a callback each time a form's submission settles without an error.
///
/// A form's reply is not a `Mutation` its controller owns, so it cannot list the queries a success
/// invalidates; this watches the session instead. It tracks `pending()` alone: a falling edge with no
/// `lastError()` is one successful submission whatever the reply text, so two identical replies (an
/// acknowledgement twice) are two successes — a watch on `lastReply()` would merge them by equality. The
/// callback runs untracked, inside the flush that saw the edge, so it may write signals and refetch queries.
class FormSuccess {
public:
    /// @param runtime The runtime the session belongs to. Borrowed: it must outlive this object.
    /// @param session The session to watch. Borrowed: it must outlive this object.
    /// @param onSuccess Runs once per successful submission.
    FormSuccess(reactive::Runtime& runtime, forms::FormSession& session, std::function<void()> onSuccess)
        : _rt{&runtime}, _session{&session}, _onSuccess{std::move(onSuccess)}, _watch{runtime, [this] { observe(); }} {}

    ~FormSuccess() = default;
    FormSuccess(FormSuccess const&) = delete;
    FormSuccess& operator=(FormSuccess const&) = delete;
    FormSuccess(FormSuccess&&) = delete;
    FormSuccess& operator=(FormSuccess&&) = delete;

private:
    void observe() {
        bool const pending = _session->pending();
        bool const settled = _wasPending && !pending;
        _wasPending = pending;
        if (!settled) {
            return;
        }
        _rt->untracked([this] {
            if (_session->lastError() == nullptr) {
                _onSuccess();
            }
        });
    }

    reactive::Runtime* _rt;
    forms::FormSession* _session;
    std::function<void()> _onSuccess;
    bool _wasPending = false;
    // Last: it runs in its constructor and reads every member above.
    reactive::Effect _watch;
};

}  // namespace morph::examples
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target examples_common_app_tests \
  && ./build/all/examples/common/app/tests/examples_common_app_tests "[form-success]"
```

Expected: PASS. Mutation check: change `bool const settled = _wasPending && !pending;` to
`bool const settled = !pending;`. Expected FAIL at the first `CHECK(successes == 0)` (the construction run counts
as a success). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/common/app/form_success.hpp examples/common/app/tests/test_form_success.cpp \
    examples/common/app/tests/CMakeLists.txt
git commit -m "wip(pastebin): FormSuccess, a callback per successful form submission

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `PasteController` — the listing, the open paste and its delete

**Files:**
- Create: `examples/pastebin/app/controllers/paste_controller.hpp`,
  `examples/pastebin/app/controllers/paste_controller.cpp`
- Create: `examples/pastebin/tests/client_fixture.hpp`
- Test: `examples/pastebin/tests/test_paste_controller.cpp`

**Interfaces:**
- Consumes: `reactive::Query<A, R>(Runtime&, BridgeHandler<M, S>&, Key, QueryOptions = {})`, `value()`,
  `error()`, `pending()`, `refetch()`; `reactive::Mutation<A, R>(Runtime&, BridgeHandler<M, S>&,
  MutationOptions)`, `run()`, `lastResult()`, `error()`, `pending()`; `MutationOptions{invalidates}`;
  `Signal`, `Computed`, `Effect`, `errorMessage`; `bridge::BridgeHandler<PasteModel>(Bridge&, IExecutor*)`;
  `pastebin::PasteModel` and its DTOs (`pastebin/dto/paste_dto.hpp`); `morph::units::toString`
  (`include/morph/util/quantity.hpp:1246`), `Quantity::hasValue` (`:748`), `Timestamp::hasValue`/`operator*`
  (`include/morph/util/datetime.hpp:397`), `DateTime::toIso8601` (`:127`); `BackendRig`, `DbFixture`, `pumpUntil`.
- Produces (Tasks 4–6 rely on these): `pastebin::client::{PasteRow, PasteFact, StatusLine, isoOrEmpty, readsText,
  visibilityText, editabilityText, rowOf, factsOf, PasteController}`; `PasteController(reactive::Runtime&,
  bridge::Bridge&, exec::IExecutor&)` with `rows()`, `countText()`, `refresh()`, `highlight()`, `highlighted()`,
  `listLoading()`, `open()`, `remove()`, `current()`, `canDelete()`, `detailTitle()`, `facts()`, `content()`,
  `statusText()`,
  `statusIsError()`, `hasStatus()`; `pastebin::testing::{ClientFixture, seedPaste, pasteOf}`.

- [ ] **Step 1: Write the failing test**

Create `examples/pastebin/tests/client_fixture.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <morph/reactive/runtime.hpp>
#include <string>
#include <utility>

#include "controllers/paste_controller.hpp"
#include "pastebin/dto/paste_dto.hpp"
#include "pastebin/models/paste_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

/// @file
/// The wiring every pastebin client test shares: one `BackendRig` client, a reactive runtime owned by the
/// rig's client executor (the executor every handler delivers on), and a `PasteController` over both.

namespace pastebin::testing {

/// @brief One client of one rig, with its controller. Members die controller first, rig last.
struct ClientFixture {
    /// @param mode The deployment shape under test.
    explicit ClientFixture(::morph::ladder::testkit::Mode mode)
        : rig{mode, 1}, runtime{*rig.executor()}, controller{runtime, rig.bridge(0), *rig.executor()} {}

    ::morph::ladder::testkit::BackendRig rig;  ///< Backend, executors and (Socket) server.
    ::morph::reactive::Runtime runtime;        ///< Owned by the rig's client executor.
    client::PasteController controller;        ///< The controller under test.
};

/// @brief A minimal valid `CreatePaste`.
/// @param content Paste body.
/// @param syntax Syntax label.
/// @return The action.
[[nodiscard]] inline CreatePaste pasteOf(std::string content, std::string syntax = "text") {
    CreatePaste action;
    action.content = std::move(content);
    action.syntax = std::move(syntax);
    return action;
}

/// @brief Stores a paste through the model directly — the process and database every rig mode shares.
/// @param action The paste to store.
/// @return Its id.
[[nodiscard]] inline std::string seedPaste(CreatePaste const& action) {
    PasteModel model;
    PasteId const created = model.execute(action).id;
    REQUIRE(created.hasValue());
    return *created;
}

}  // namespace pastebin::testing
```

Create `examples/pastebin/tests/test_paste_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <vector>

#include "client_fixture.hpp"
#include "clock.hpp"
#include "controllers/paste_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using pastebin::client::PasteController;
using pastebin::client::PasteFact;
using pastebin::client::PasteRow;
using pastebin::testing::ClientFixture;
using pastebin::testing::pasteOf;
using pastebin::testing::seedPaste;

/// @brief Whether a row type carries a paste's body.
template <class Row>
concept CarriesContent = requires(Row const& row) { row.content; };

[[nodiscard]] std::vector<std::string> keysOf(std::vector<PasteFact> const& facts) {
    std::vector<std::string> keys;
    for (auto const& fact : facts) {
        keys.push_back(fact.key);
    }
    return keys;
}

[[nodiscard]] std::string lineFor(PasteController const& controller, std::string const& key) {
    for (auto const& fact : controller.facts()) {
        if (fact.key == key) {
            return fact.line;
        }
    }
    return {};
}

[[nodiscard]] bool listed(PasteController const& controller, std::string const& pasteId) {
    return std::ranges::any_of(controller.rows(), [&](PasteRow const& row) { return row.pasteId == pasteId; });
}

void openAndWait(PasteController& controller, std::string const& pasteId) {
    controller.open(pasteId);
    REQUIRE(pumpUntil([&] { return controller.detailTitle() == pasteId; }));
}

}  // namespace

// A listing never carries a paste's body; the row type cannot hold one.
static_assert(!CarriesContent<PasteRow>);

TEST_CASE("PasteController lists the public pastes in the narrower row shape, all three backend modes",
          "[pastebin][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    std::vector<std::string> created;
    for (int i = 0; i < 3; ++i) {
        created.push_back(seedPaste(pasteOf("listed " + std::to_string(i))));
    }
    auto hidden = pasteOf("hidden");
    hidden.visibility = pastebin::Visibility::Private;
    std::string const privateId = seedPaste(hidden);

    ClientFixture client{mode};
    REQUIRE(pumpUntil([&] { return client.controller.rows().size() == 3; }));
    for (auto const& pasteId : created) {
        CHECK(listed(client.controller, pasteId));
    }
    CHECK_FALSE(listed(client.controller, privateId));
    CHECK(client.controller.countText() == "3 public paste(s)");
    for (auto const& row : client.controller.rows()) {
        CHECK(row.visibility == "Public");
        CHECK(row.syntax == "text");
        CHECK(row.createdAt.find('T') != std::string::npos);
    }
}

TEST_CASE("PasteController::open shows every fact of the paste and consumes one read, all three backend modes",
          "[pastebin][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    std::string const pasteId = seedPaste(pasteOf("bag contents", "cpp"));
    ClientFixture client{mode};
    CHECK_FALSE(client.controller.canDelete());

    openAndWait(client.controller, pasteId);
    auto const& controller = client.controller;
    CHECK(keysOf(controller.facts()) ==
          std::vector<std::string>{"syntax", "visibility", "editability", "created", "expires", "reads", "burn after"});
    CHECK(lineFor(controller, "syntax") == "syntax: cpp");
    CHECK(lineFor(controller, "visibility") == "visibility: Public");
    CHECK(lineFor(controller, "editability") == "editability: Immutable");
    CHECK(lineFor(controller, "expires") == "expires: never");
    CHECK(lineFor(controller, "reads") == "reads: 1");
    CHECK(lineFor(controller, "burn after") == "burn after: no limit");
    CHECK(lineFor(controller, "created").find('T') != std::string::npos);
    CHECK(controller.content() == "bag contents");
    CHECK(controller.statusText() == "opened " + pasteId + ", read 1 time(s)");
    CHECK_FALSE(controller.statusIsError());
    CHECK(controller.canDelete());
}

TEST_CASE("PasteController renders the engaged arm of every formatted fact", "[pastebin][controller]") {
    DbFixture fixture;
    auto action = pasteOf("fully engaged", "cpp");
    action.expiresAt = ::morph::time::Timestamp{*morph::ladder::now() + std::chrono::hours{24}};
    action.burnAfterReads = pastebin::Reads{::morph::math::Rational{9, pastebin::Reads::declaredPrecision()}};
    action.visibility = pastebin::Visibility::Private;
    action.editability = pastebin::Editability::Editable;
    std::string const pasteId = seedPaste(action);
    ClientFixture client{Mode::Local};

    openAndWait(client.controller, pasteId);
    CHECK(lineFor(client.controller, "visibility") == "visibility: Private");
    CHECK(lineFor(client.controller, "editability") == "editability: Editable");
    std::string const expires = lineFor(client.controller, "expires");
    CHECK(expires.find('T') != std::string::npos);
    CHECK(expires.back() == 'Z');
    CHECK(lineFor(client.controller, "burn after") == "burn after: 9");
    // Private: opened by id, never listed.
    CHECK_FALSE(listed(client.controller, pasteId));
}

TEST_CASE("Opening the last allowed read burns the paste, and the listing drops it, all three backend modes",
          "[pastebin][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto action = pasteOf("one read only");
    action.burnAfterReads = pastebin::Reads::fromDouble(1.0);
    std::string const pasteId = seedPaste(action);
    ClientFixture client{mode};
    REQUIRE(pumpUntil([&] { return listed(client.controller, pasteId); }));

    openAndWait(client.controller, pasteId);
    CHECK(lineFor(client.controller, "reads") == "reads: 1");
    // GetPaste invalidates the listing: the burn shows without a manual refresh.
    REQUIRE(pumpUntil([&] { return !listed(client.controller, pasteId); }));
}

TEST_CASE("Refreshing the list never re-reads the open paste", "[pastebin][controller]") {
    DbFixture fixture;
    auto action = pasteOf("three reads");
    action.burnAfterReads = pastebin::Reads::fromDouble(3.0);
    std::string const pasteId = seedPaste(action);
    ClientFixture client{Mode::Local};

    openAndWait(client.controller, pasteId);
    REQUIRE(lineFor(client.controller, "reads") == "reads: 1");
    for (int i = 0; i < 3; ++i) {
        client.controller.refresh();
        REQUIRE(client.controller.listLoading());
        REQUIRE(pumpUntil([&] { return !client.controller.listLoading(); }));
        CHECK(listed(client.controller, pasteId));
    }
    // The next explicit open is the second read: three refreshes consumed none.
    client.controller.open(pasteId);
    REQUIRE(pumpUntil([&] { return lineFor(client.controller, "reads") == "reads: 2"; }));
}

TEST_CASE("PasteController::remove deletes the open paste, and opening it again fails, all three backend modes",
          "[pastebin][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    std::string const pasteId = seedPaste(pasteOf("doomed"));
    ClientFixture client{mode};
    openAndWait(client.controller, pasteId);

    client.controller.remove();
    REQUIRE(pumpUntil([&] { return !client.controller.current().has_value() && client.controller.rows().empty(); }));
    CHECK(client.controller.statusText() == "deleted");
    CHECK_FALSE(client.controller.canDelete());

    client.controller.open(pasteId);
    REQUIRE(pumpUntil([&] { return client.controller.statusIsError(); }));
    CHECK(client.controller.statusText().find("GetPaste") != std::string::npos);
    CHECK_FALSE(client.controller.current().has_value());
}

TEST_CASE("An unknown id reports the model's message, and remove() with nothing open issues nothing",
          "[pastebin][controller]") {
    DbFixture fixture;
    ClientFixture client{Mode::Local};
    client.controller.remove();
    CHECK_FALSE(client.controller.hasStatus());

    client.controller.open("no-such-paste");
    REQUIRE(pumpUntil([&] { return client.controller.statusIsError(); }));
    CHECK_FALSE(client.controller.statusText().empty());
    CHECK_FALSE(client.controller.current().has_value());
}

TEST_CASE("A failed listing reports its error instead of crashing", "[pastebin][controller]") {
    DbFixture fixture;
    ClientFixture client{Mode::Local};
    REQUIRE(pumpUntil([&] { return !client.controller.listLoading(); }));
    CHECK(client.controller.countText() == "0 public paste(s)");
    {
        // A genuine store error, through the schema: DbFixture re-creates it for the next case.
        ::Lightweight::SqlStatement stmt;
        static_cast<void>(stmt.ExecuteDirect("DROP TABLE pastes"));
    }
    client.controller.refresh();
    REQUIRE(pumpUntil([&] { return client.controller.statusIsError(); }));
    CHECK_FALSE(client.controller.statusText().empty());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_pastebin_tests`
Expected: FAIL — `'controllers/paste_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/pastebin/app/controllers/paste_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <vector>

#include "pastebin/dto/paste_dto.hpp"
#include "pastebin/models/paste_model.hpp"

/// @file
/// The controller behind pastebin's one screen: the public listing, the open paste and its delete, and the
/// create form. Toolkit-free; the view binds to its accessors and formats nothing itself.

namespace pastebin::client {

/// @brief One public paste in the listing, formatted for display. It carries no content: a listing never
///        shows a paste's body, which is why `PasteSummary` is narrower than `PasteView`.
struct PasteRow {
    std::string pasteId;     ///< The animal-name id; also the row's key.
    std::string syntax;      ///< The syntax label.
    std::string visibility;  ///< "Public" or "Private".
    std::string createdAt;   ///< ISO-8601 creation instant.

    /// @brief Field-wise equality, so an unchanged listing does not re-render.
    bool operator==(PasteRow const&) const = default;
};

/// @brief One labelled fact about the open paste, with the line the view shows.
struct PasteFact {
    std::string key;   ///< The fact's name; also its row key.
    std::string line;  ///< "key: value".

    /// @brief Field-wise equality.
    bool operator==(PasteFact const&) const = default;
};

/// @brief The screen's status line: the last list, open, delete or create outcome.
struct StatusLine {
    std::string text;      ///< Empty when there is nothing to report.
    bool isError = false;  ///< Whether `text` reports a failure.

    /// @brief Field-wise equality.
    bool operator==(StatusLine const&) const = default;
};

/// @brief An instant as ISO-8601, or empty when unset.
/// @param instant The instant.
/// @return Its text.
[[nodiscard]] std::string isoOrEmpty(::morph::time::Timestamp const& instant);

/// @brief A read count as text (`morph::units::toString`, so `"N/A"` when unset).
/// @param reads The count.
/// @return Its text.
[[nodiscard]] std::string readsText(Reads const& reads);

/// @brief "Public" or "Private".
/// @param visibility The visibility.
/// @return Its word.
[[nodiscard]] std::string visibilityText(Visibility visibility);

/// @brief "Immutable" or "Editable".
/// @param editability The editability.
/// @return Its word.
[[nodiscard]] std::string editabilityText(Editability editability);

/// @brief One listing row.
/// @param summary The row as `ListPastes` returned it.
/// @return The display row.
[[nodiscard]] PasteRow rowOf(PasteSummary const& summary);

/// @brief The open paste's facts, in display order: syntax, visibility, editability, created, expires
///        ("never" when unset), reads, burn after ("no limit" when unset).
/// @param view The paste.
/// @return The facts.
[[nodiscard]] std::vector<PasteFact> factsOf(PasteView const& view);

/// @brief pastebin's screen controller.
///
/// The listing is a `Query<ListPastes>`, fetched on construction and on `refresh()`. Opening a paste is a
/// `Mutation<GetPaste>`, never a query: `GetPaste` consumes one of the paste's allowed reads, so only an
/// explicit `open()` may issue it — never a refetch, a re-render or a selection change. A successful open
/// and a successful delete each invalidate the listing, which is how a burn shows up without a manual
/// refresh. Highlighting a row (the table's selection) is separate from opening it for the same reason.
class PasteController {
public:
    /// @param runtime The runtime; its owner must be @p callbacks.
    /// @param bridge The client's bridge.
    /// @param callbacks Where replies are delivered.
    PasteController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                    ::morph::exec::IExecutor& callbacks);

    ~PasteController() = default;
    PasteController(PasteController const&) = delete;
    PasteController& operator=(PasteController const&) = delete;
    PasteController(PasteController&&) = delete;
    PasteController& operator=(PasteController&&) = delete;

    /// @brief The public listing. Tracked.
    /// @return Its rows, newest first; empty before the first reply.
    [[nodiscard]] std::vector<PasteRow> const& rows() const { return _rows.get(); }

    /// @brief "N public paste(s)". Tracked.
    /// @return The count line.
    [[nodiscard]] std::string const& countText() const { return _countText.get(); }

    /// @brief Whether a listing request is in flight. Tracked.
    /// @return True from issue to reply.
    [[nodiscard]] bool listLoading() const { return _list.pending(); }

    /// @brief Fetches the listing again. Never re-reads the open paste.
    void refresh() { _list.refetch(); }

    /// @brief Highlights a row without opening it.
    /// @param pasteId The row, or `nullopt` for none.
    void highlight(std::optional<std::string> pasteId) { _highlighted.set(std::move(pasteId)); }

    /// @brief The highlighted row. Tracked.
    /// @return Its id, or `nullopt`.
    [[nodiscard]] std::optional<std::string> const& highlighted() const { return _highlighted.get(); }

    /// @brief Opens a paste, consuming one of its reads.
    /// @param pasteId The paste.
    void open(std::string pasteId);

    /// @brief Deletes the open paste; does nothing when none is open.
    void remove();

    /// @brief The open paste. Tracked.
    /// @return It, or `nullopt`.
    [[nodiscard]] std::optional<PasteView> const& current() const { return _current.get(); }

    /// @brief Whether the delete button acts. Tracked.
    /// @return True while a paste is open and no delete is in flight.
    [[nodiscard]] bool canDelete() const { return _canDelete.get(); }

    /// @brief The detail pane's title: the open paste's id, or a hint. Tracked.
    /// @return The title.
    [[nodiscard]] std::string const& detailTitle() const { return _detailTitle.get(); }

    /// @brief The open paste's facts. Tracked.
    /// @return `factsOf(*current())`, or empty.
    [[nodiscard]] std::vector<PasteFact> const& facts() const { return _facts.get(); }

    /// @brief The open paste's content. Tracked.
    /// @return The body, or empty.
    [[nodiscard]] std::string const& content() const { return _content.get(); }

    /// @brief The status line's text. Tracked.
    /// @return The text, or empty.
    [[nodiscard]] std::string const& statusText() const { return _status.get().text; }

    /// @brief Whether the status line reports a failure. Tracked.
    /// @return True for a failure.
    [[nodiscard]] bool statusIsError() const { return _status.get().isError; }

    /// @brief Whether there is a status to show. Tracked.
    /// @return True when the text is non-empty.
    [[nodiscard]] bool hasStatus() const { return !_status.get().text.empty(); }

private:
    void reportError(std::exception_ptr const& error);
    void reportInfo(std::string text);

    ::morph::reactive::Runtime* _rt;
    ::morph::bridge::BridgeHandler<PasteModel> _handler;
    ::morph::reactive::Signal<std::optional<std::string>> _highlighted;
    ::morph::reactive::Signal<std::optional<PasteView>> _current;
    ::morph::reactive::Signal<StatusLine> _status;
    ::morph::reactive::Query<ListPastes> _list;
    ::morph::reactive::Mutation<GetPaste> _open;
    ::morph::reactive::Mutation<DeletePaste> _delete;
    ::morph::reactive::Computed<std::vector<PasteRow>> _rows;
    ::morph::reactive::Computed<std::string> _countText;
    ::morph::reactive::Computed<bool> _canDelete;
    ::morph::reactive::Computed<std::string> _detailTitle;
    ::morph::reactive::Computed<std::vector<PasteFact>> _facts;
    ::morph::reactive::Computed<std::string> _content;
    ::morph::reactive::Effect _onListFailed;
    ::morph::reactive::Effect _onOpened;
    ::morph::reactive::Effect _onOpenFailed;
    ::morph::reactive::Effect _onDeleted;
    ::morph::reactive::Effect _onDeleteFailed;
};

}  // namespace pastebin::client
```

Create `examples/pastebin/app/controllers/paste_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/paste_controller.hpp"

#include <morph/util/quantity.hpp>
#include <string>
#include <utility>

namespace pastebin::client {

std::string isoOrEmpty(::morph::time::Timestamp const& instant) {
    return instant.hasValue() ? (*instant).toIso8601() : std::string{};
}

std::string readsText(Reads const& reads) { return ::morph::units::toString(reads); }

std::string visibilityText(Visibility visibility) {
    return visibility == Visibility::Private ? "Private" : "Public";
}

std::string editabilityText(Editability editability) {
    return editability == Editability::Editable ? "Editable" : "Immutable";
}

PasteRow rowOf(PasteSummary const& summary) {
    return PasteRow{.pasteId = summary.id.hasValue() ? *summary.id : std::string{},
                    .syntax = summary.syntax,
                    .visibility = visibilityText(summary.visibility),
                    .createdAt = isoOrEmpty(summary.createdAt)};
}

std::vector<PasteFact> factsOf(PasteView const& view) {
    auto const fact = [](std::string const& key, std::string const& value) {
        return PasteFact{.key = key, .line = key + ": " + value};
    };
    std::string const expires = isoOrEmpty(view.expiresAt);
    return {fact("syntax", view.syntax),
            fact("visibility", visibilityText(view.visibility)),
            fact("editability", editabilityText(view.editability)),
            fact("created", isoOrEmpty(view.createdAt)),
            fact("expires", expires.empty() ? std::string{"never"} : expires),
            fact("reads", readsText(view.readCount)),
            fact("burn after",
                 view.burnAfterReads.hasValue() ? readsText(view.burnAfterReads) : std::string{"no limit"})};
}

PasteController::PasteController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                                 ::morph::exec::IExecutor& callbacks)
    : _rt{&runtime},
      _handler{bridge, &callbacks},
      _highlighted{runtime, std::nullopt},
      _current{runtime, std::nullopt},
      _status{runtime, StatusLine{}},
      _list{runtime, _handler, [] { return std::optional<ListPastes>{ListPastes{}}; }},
      _open{runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_list}}},
      _delete{runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_list}}},
      _rows{runtime,
            [this] {
                std::vector<PasteRow> rows;
                if (auto const& page = _list.value()) {
                    rows.reserve(page->pastes.size());
                    for (auto const& summary : page->pastes) {
                        rows.push_back(rowOf(summary));
                    }
                }
                return rows;
            }},
      _countText{runtime, [this] { return std::to_string(_rows.get().size()) + " public paste(s)"; }},
      _canDelete{runtime, [this] { return _current.get().has_value() && !_delete.pending(); }},
      _detailTitle{runtime,
                   [this] {
                       auto const& paste = _current.get();
                       return paste.has_value() && paste->id.hasValue() ? *paste->id
                                                                        : std::string{"no paste open: activate one in the list"};
                   }},
      _facts{runtime,
             [this] {
                 auto const& paste = _current.get();
                 return paste.has_value() ? factsOf(*paste) : std::vector<PasteFact>{};
             }},
      _content{runtime,
               [this] {
                   auto const& paste = _current.get();
                   return paste.has_value() ? paste->content : std::string{};
               }},
      _onListFailed{runtime, [this] { reportError(_list.error()); }},
      _onOpened{runtime,
                [this] {
                    auto const& opened = _open.lastResult();
                    if (!opened.has_value()) {
                        return;
                    }
                    _rt->untracked([&] {
                        _current.set(*opened);
                        std::string const pasteId = opened->id.hasValue() ? *opened->id : std::string{};
                        reportInfo("opened " + pasteId + ", read " + readsText(opened->readCount) + " time(s)");
                    });
                }},
      _onOpenFailed{runtime, [this] { reportError(_open.error()); }},
      _onDeleted{runtime,
                 [this] {
                     if (!_delete.lastResult().has_value()) {
                         return;
                     }
                     _rt->untracked([&] {
                         _current.set(std::nullopt);
                         _highlighted.set(std::nullopt);
                         reportInfo("deleted");
                     });
                 }},
      _onDeleteFailed{runtime, [this] { reportError(_delete.error()); }} {}

void PasteController::open(std::string pasteId) { _open.run(GetPaste{.id = PasteId{std::move(pasteId)}}); }

void PasteController::remove() {
    auto const& paste = _current.peek();
    if (!paste.has_value() || !paste->id.hasValue()) {
        return;
    }
    _delete.run(DeletePaste{.id = paste->id});
}

void PasteController::reportError(std::exception_ptr const& error) {
    if (error != nullptr) {
        _status.set(StatusLine{.text = ::morph::reactive::errorMessage(error), .isError = true});
    }
}

void PasteController::reportInfo(std::string text) { _status.set(StatusLine{.text = std::move(text), .isError = false}); }

}  // namespace pastebin::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_pastebin_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/pastebin/ladder_pastebin_tests "[pastebin][controller]"
```

Expected: PASS. Mutation check: in the constructor, change
`_open{runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_list}}}`
to `_open{runtime, _handler}`. Expected FAIL in "Opening the last allowed read burns the paste, and the listing drops
it" (the wait for the row to disappear times out). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/pastebin/app examples/pastebin/tests/client_fixture.hpp examples/pastebin/tests/test_paste_controller.cpp
git commit -m "wip(pastebin): PasteController — listing, explicit open, delete

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 4: The typed create form

**Files:**
- Modify: `examples/pastebin/app/controllers/paste_controller.hpp` — includes, two public accessors after
  `hasStatus()`, a private method and three members
- Modify: `examples/pastebin/app/controllers/paste_controller.cpp` — three constructor initialisers, `onCreated()`
- Modify: `examples/pastebin/tests/client_fixture.hpp` — `createVia()`
- Test: `examples/pastebin/tests/test_paste_controller.cpp` — four cases appended

**Interfaces:**
- Consumes: `forms::Form<A, M, S>(Runtime&, BridgeHandler<M, S>&, ChoiceFetcher, FormSessionOptions = {})`,
  `session()`, `set<Member>()`, `ready()`, `lastResult()` (`typed_form.hpp`); `forms::handlerChoiceFetcher(IExecutor&,
  Handlers&...)` (`handler_submitter.hpp`, Part 5 Task 10b); `FormSession::submit()`, `reset()`, `ready()`,
  `body()`, `pending()`, `lastError()`; `examples::FormSuccess` (Task 2).
- Produces: `PasteController::createForm() -> forms::Form<CreatePaste, PasteModel>&`,
  `PasteController::lastCreatedId() -> std::optional<std::string> const&`; `pastebin::testing::createVia()`.

- [ ] **Step 1: Write the failing test**

Append to `examples/pastebin/tests/client_fixture.hpp`, inside `namespace pastebin::testing`, after `seedPaste`:

```cpp
/// @brief Fills the create form's two required fields, submits it and waits for the new id.
/// @param controller The controller whose form to use.
/// @param content Paste body.
/// @param syntax Syntax label.
/// @return The new paste's id.
[[nodiscard]] inline std::string createVia(client::PasteController& controller, std::string content,
                                           std::string syntax = "text") {
    auto& form = controller.createForm();
    form.set<&CreatePaste::content>(std::move(content));
    form.set<&CreatePaste::syntax>(std::move(syntax));
    REQUIRE(form.ready());
    std::optional<std::string> const before = controller.lastCreatedId();
    form.session().submit();
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return controller.lastCreatedId() != before; }));
    return controller.lastCreatedId().value_or(std::string{});
}
```

and add `#include <optional>` to its includes.

Append to `examples/pastebin/tests/test_paste_controller.cpp`:

```cpp
TEST_CASE("The create form stores pastes, lists them and resets itself, all three backend modes",
          "[pastebin][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    ClientFixture client{mode};
    std::vector<std::string> created;
    for (int i = 0; i < 3; ++i) {
        created.push_back(pastebin::testing::createVia(client.controller, "created " + std::to_string(i)));
    }
    REQUIRE(pumpUntil([&] { return client.controller.rows().size() == 3; }));
    for (auto const& pasteId : created) {
        CHECK_FALSE(pasteId.empty());
        CHECK(listed(client.controller, pasteId));
    }
    CHECK(client.controller.statusText() == "created " + created.back());
    // reset(): the required fields are empty again, so the form waits for new input.
    CHECK_FALSE(client.controller.createForm().ready());
}

TEST_CASE("The create form is not ready while CreatePaste::validate refuses, and submitting it sends nothing",
          "[pastebin][controller]") {
    DbFixture fixture;
    ClientFixture client{Mode::Local};
    auto& form = client.controller.createForm();
    form.set<&pastebin::CreatePaste::syntax>(std::string{"text"});
    CHECK_FALSE(form.ready());  // content is required

    form.set<&pastebin::CreatePaste::content>(std::string{"body"});
    form.set<&pastebin::CreatePaste::syntax>(std::string(pastebin::kMaxSyntaxBytes + 1, 'x'));
    // The schema carries no length bound for syntax; CreatePaste::validate() does, and it gates the session itself.
    CHECK_FALSE(form.ready());
    CHECK_FALSE(form.session().ready());
    CHECK_FALSE(form.session().body().has_value());

    form.session().submit();
    CHECK_FALSE(form.session().pending());  // nothing went out
    REQUIRE(pumpUntil([&] { return !client.controller.listLoading(); }));
    CHECK(form.session().lastError() == nullptr);
    CHECK(client.controller.rows().empty());
    CHECK_FALSE(client.controller.lastCreatedId().has_value());

    form.set<&pastebin::CreatePaste::syntax>(std::string{"text"});
    CHECK(form.ready());  // the same draft, once validate() accepts it
}

TEST_CASE("A private paste is created and opened by id, but never listed", "[pastebin][controller]") {
    DbFixture fixture;
    ClientFixture client{Mode::Local};
    auto& form = client.controller.createForm();
    form.set<&pastebin::CreatePaste::visibility>(pastebin::Visibility::Private);
    std::string const pasteId = pastebin::testing::createVia(client.controller, "hidden");
    REQUIRE(pumpUntil([&] { return !client.controller.listLoading(); }));
    CHECK(client.controller.rows().empty());

    openAndWait(client.controller, pasteId);
    CHECK(lineFor(client.controller, "visibility") == "visibility: Private");
}

TEST_CASE("a controller destroyed with a create in flight drops its reply", "[pastebin][controller]") {
    // LocalSingleThread: every dispatch and completion runs on the rig's one executor, so the abandoned
    // create is strictly ordered before the live one, and its reply reaches the dead controller's gate
    // before the live reply arrives. ASan is the observer: without the gate the reply writes freed storage.
    DbFixture fixture;
    morph::ladder::testkit::BackendRig rig{Mode::LocalSingleThread, 1};
    morph::reactive::Runtime runtime{*rig.executor()};
    {
        PasteController abandoned{runtime, rig.bridge(0), *rig.executor()};
        auto& form = abandoned.createForm();
        form.set<&pastebin::CreatePaste::content>(std::string{"submitted, then abandoned"});
        form.set<&pastebin::CreatePaste::syntax>(std::string{"text"});
        REQUIRE(form.ready());
        form.session().submit();
        // No pump before this brace: the reply cannot have been delivered yet.
    }
    PasteController live{runtime, rig.bridge(0), *rig.executor()};
    static_cast<void>(pastebin::testing::createVia(live, "submitted and awaited"));
    // The abandoned create ran on the model; only its delivery was dropped.
    REQUIRE(pumpUntil([&] { return live.rows().size() == 2; }));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_pastebin_tests`
Expected: FAIL — `no member named 'createForm' in 'pastebin::client::PasteController'`.

- [ ] **Step 3: Implement**

In `examples/pastebin/app/controllers/paste_controller.hpp`, add after `#include <morph/core/executor.hpp>`:

```cpp
#include <morph/forms/engine/handler_submitter.hpp>
#include <morph/forms/engine/typed_form.hpp>
```

and after `#include "pastebin/models/paste_model.hpp"`:

```cpp
#include "app/form_success.hpp"
```

Add to the class doc comment, before its closing line:

```cpp
/// The create form is a typed `forms::Form<CreatePaste>` on the controller's own handler; its submission is
/// explicit (`CreatePaste::explicitSubmit`), `CreatePaste::validate()` gates its readiness, and a success lists the
/// new paste, reports its id and resets the form.
```

After `hasStatus()`, add:

```cpp
    /// @brief The typed create form; the view renders its session.
    /// @return The form.
    [[nodiscard]] ::morph::forms::Form<CreatePaste, PasteModel>& createForm() noexcept { return _create; }

    /// @brief The id the last successful create returned. Tracked.
    /// @return It, or `nullopt` before the first.
    [[nodiscard]] std::optional<std::string> const& lastCreatedId() const { return _lastCreated.get(); }
```

After `void reportInfo(std::string text);` add `void onCreated();`. Add the member
`::morph::reactive::Signal<std::optional<std::string>> _lastCreated;` directly after `_status`, the member
`::morph::forms::Form<CreatePaste, PasteModel> _create;` directly after `_delete`, and as the **last** member:

```cpp
    // Last: it watches `_create` and calls into everything above.
    ::morph::examples::FormSuccess _onCreated;
```

In `paste_controller.cpp`, add the initialisers in declaration order — after `_status{runtime, StatusLine{}},`:

```cpp
      _lastCreated{runtime, std::nullopt},
```

after `_delete{runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_list}}},` (the fetcher goes
through the same handler; `CreatePaste` names no Choice today, and one added later must reach this model too):

```cpp
      _create{runtime, _handler, ::morph::forms::handlerChoiceFetcher(callbacks, _handler)},
```

and replace the final `_onDeleteFailed{runtime, [this] { reportError(_delete.error()); }} {}` with:

```cpp
      _onDeleteFailed{runtime, [this] { reportError(_delete.error()); }},
      _onCreated{runtime, _create.session(), [this] { onCreated(); }} {}
```

Add after `PasteController::remove()`:

```cpp
void PasteController::onCreated() {
    auto const& result = _create.lastResult();
    std::string const created = result.has_value() && result->id.hasValue() ? *result->id : std::string{};
    _lastCreated.set(created);
    reportInfo("created " + created);
    _list.refetch();
    _create.session().reset();
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_pastebin_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/pastebin/ladder_pastebin_tests "[pastebin][controller]"
```

Expected: PASS. Mutation check: delete `_list.refetch();` from `onCreated()`. Expected FAIL in "The create form
stores pastes, lists them and resets itself" (the wait for three rows times out). Restore. Second mutation
check, on the ASan build of Task 8: a typed form's reply reaches its session through `Form::execute`'s relay, gated
by the form's `_relay` scope, so that is the gate this case exercises. In `include/morph/forms/engine/typed_form.hpp`,
change `Form::execute`'s `.then(_relay,` to `.thenDetached(` and its `.onError(_relay,` to `.onErrorDetached(`;
expected `AddressSanitizer: heap-use-after-free` in "a controller destroyed with a create in flight drops its
reply" (the relay writes the destroyed form's `_lastResult`). Restore. If the case stays clean under that mutation,
the destroyed `BridgeHandler` dropped the reply first: say so in the hand-off instead of reporting the form's gate
as observed.

- [ ] **Step 5: Commit**

```bash
git add examples/pastebin/app examples/pastebin/tests/client_fixture.hpp examples/pastebin/tests/test_paste_controller.cpp
git commit -m "wip(pastebin): the typed CreatePaste form

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: `pasteScreen`

**Files:**
- Create: `examples/pastebin/app/views/paste_view.hpp`, `examples/pastebin/app/views/paste_view.cpp`
- Test: `examples/pastebin/tests/test_paste_view.cpp`

**Interfaces:**
- Consumes: `ui::{Node, Key, text, button, column, row, panel, scroll, busy, table, forEach, TableColumn,
  TableOptions, SelectionMode, Sizing, TextRole}` (Part 2 `view.hpp`); `ui::Mounted` (`mount.hpp`);
  `ui::testing::RecordingBackend::{find, all, prop, click, selectRows, activateRow}`;
  `forms::formView(FormSession&, FormViewOptions)`, `FormViewOptions{overrides, gridColumns, submitLabel,
  flatGridColumns}` (`form_view.hpp`; with `gridColumns = 1` the implicit group is one column too);
  `PasteController` (Tasks 3–4).
- Produces: `pastebin::client::pasteScreen(PasteController&) -> ui::Node`.

- [ ] **Step 1: Write the failing test**

Create `examples/pastebin/tests/test_paste_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <string>

#include "client_fixture.hpp"
#include "testkit/db_fixture.hpp"
#include "views/paste_view.hpp"

namespace {

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using morph::ui::testing::RecordingBackend;
using pastebin::testing::ClientFixture;
using pastebin::testing::pasteOf;
using pastebin::testing::seedPaste;

}  // namespace

TEST_CASE("pasteScreen: the tree, and its refresh drives the listing", "[pastebin][view]") {
    DbFixture fixture;
    std::string const pasteId = seedPaste(pasteOf("viewed", "cpp"));
    ClientFixture client{Mode::Local};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, pastebin::client::pasteScreen(client.controller)};
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", pasteId).has_value(); }));

    CHECK(backend.find("Panel", "title", "New paste").has_value());
    CHECK(backend.find("Button", "label", "Create paste").has_value());
    CHECK(backend.find("Text", "text", "1 public paste(s)").has_value());
    CHECK(backend.find("Panel", "title", "no paste open: activate one in the list").has_value());
    auto const remove = backend.find("Button", "label", "Delete this paste");
    REQUIRE(remove.has_value());
    CHECK(backend.prop(*remove, "enabled") == "false");
    CHECK(backend.all("Table").size() == 1);

    static_cast<void>(seedPaste(pasteOf("second")));
    auto const refresh = backend.find("Button", "label", "Refresh list");
    REQUIRE(refresh.has_value());
    backend.click(*refresh);
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "2 public paste(s)").has_value(); }));
}

TEST_CASE("pasteScreen: moving the selection opens nothing, activating a row opens it once", "[pastebin][view]") {
    DbFixture fixture;
    auto action = pasteOf("two reads");
    action.burnAfterReads = pastebin::Reads::fromDouble(2.0);
    std::string const pasteId = seedPaste(action);
    ClientFixture client{Mode::Local};
    RecordingBackend backend;
    morph::ui::Mounted const mounted{client.runtime, backend, pastebin::client::pasteScreen(client.controller)};
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", pasteId).has_value(); }));
    auto const tables = backend.all("Table");
    REQUIRE(tables.size() == 1);

    backend.selectRows(tables.front(), {morph::ui::Key{pasteId}});
    REQUIRE(pumpUntil([&] { return client.controller.highlighted() == std::optional<std::string>{pasteId}; }));
    CHECK_FALSE(client.controller.current().has_value());

    backend.activateRow(tables.front(), morph::ui::Key{pasteId});
    // One read: had the selection opened the paste too, this would say 2.
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "reads: 1").has_value(); }));
    CHECK(backend.find("Panel", "title", pasteId).has_value());

    auto const remove = backend.find("Button", "label", "Delete this paste");
    REQUIRE(remove.has_value());
    REQUIRE(pumpUntil([&] { return backend.prop(*remove, "enabled") == "true"; }));
    backend.click(*remove);
    REQUIRE(pumpUntil([&] { return client.controller.rows().empty() && !client.controller.current().has_value(); }));
    CHECK(backend.find("Text", "text", "deleted").has_value());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_pastebin_tests`
Expected: FAIL — `'views/paste_view.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/pastebin/app/views/paste_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/paste_controller.hpp"

/// @file
/// pastebin's one screen as a view tree: bindings over `PasteController`, and nothing else.

namespace pastebin::client {

/// @brief The create form, the listing and the open paste side by side, under a status line.
///
/// Selecting a row only highlights it; activating it (Enter, a double click) opens it, because opening
/// consumes one of the paste's reads.
/// @param controller The screen's controller. Borrowed: it must outlive the mounted tree.
/// @return The screen.
[[nodiscard]] ::morph::ui::Node pasteScreen(PasteController& controller);

}  // namespace pastebin::client
```

Create `examples/pastebin/app/views/paste_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/paste_view.hpp"

#include <morph/forms/engine/form_view.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pastebin::client {

namespace {

namespace ui = ::morph::ui;

/// @brief A table key as the paste id it stands for.
[[nodiscard]] std::optional<std::string> pasteIdOf(ui::Key const& key) {
    if (auto const* pasteId = std::get_if<std::string>(&key)) {
        return *pasteId;
    }
    return std::nullopt;
}

/// @brief The highlighted paste as the table's selection.
[[nodiscard]] std::vector<ui::Key> selectionOf(std::optional<std::string> const& pasteId) {
    return pasteId.has_value() ? std::vector<ui::Key>{ui::Key{*pasteId}} : std::vector<ui::Key>{};
}

[[nodiscard]] ui::Node statusLine(PasteController& controller) {
    return ui::text({.text = [&controller] { return controller.statusText(); },
                     .role = [&controller] {
                         return controller.statusIsError() ? ui::TextRole::Error : ui::TextRole::Success;
                     },
                     .common = {.visible = [&controller] { return controller.hasStatus(); }}});
}

[[nodiscard]] ui::Node pasteTable(PasteController& controller) {
    return ui::table<PasteRow>(
        {ui::TableColumn{.label = "Paste"}, ui::TableColumn{.label = "Syntax"},
         ui::TableColumn{.label = "Visibility"}, ui::TableColumn{.label = "Created", .width = ui::Sizing::stretch()}},
        [&controller] { return controller.rows(); }, [](PasteRow const& row) { return ui::Key{row.pasteId}; },
        [](::morph::reactive::Signal<PasteRow> const& row) {
            return std::vector<ui::Node>{ui::text({.text = [&row] { return row.get().pasteId; }}),
                                         ui::text({.text = [&row] { return row.get().syntax; }}),
                                         ui::text({.text = [&row] { return row.get().visibility; }}),
                                         ui::text({.text = [&row] { return row.get().createdAt; }})};
        },
        ui::TableOptions{
            .selectionMode = ui::SelectionMode::Single,
            .selection = [&controller] { return selectionOf(controller.highlighted()); },
            .onSelectionChange =
                [&controller](std::vector<ui::Key> const& keys) {
                    controller.highlight(keys.empty() ? std::nullopt : pasteIdOf(keys.front()));
                },
            .onActivate =
                [&controller](ui::Key const& key) {
                    if (auto pasteId = pasteIdOf(key)) {
                        controller.open(std::move(*pasteId));
                    }
                },
            .common = {.layout = {.height = ui::Sizing::stretch()}}});
}

[[nodiscard]] ui::Node listPane(PasteController& controller) {
    return ui::column(
        {.children = {ui::panel({.title = "New paste",
                                 .padding = 1,
                                 .child = ::morph::forms::formView(
                                     controller.createForm().session(),
                                     {.overrides = {}, .gridColumns = 1, .submitLabel = "Create paste"})}),
                      ui::row({.children = {ui::button({.label = "Refresh list",
                                                        .onClick = [&controller] { controller.refresh(); }}),
                                            ui::busy({.active = [&controller] { return controller.listLoading(); },
                                                      .label = "loading"}),
                                            ui::text({.text = [&controller] { return controller.countText(); },
                                                      .role = ui::TextRole::Muted})},
                               .gap = 2}),
                      pasteTable(controller)},
         .gap = 1,
         .common = {.layout = {.width = ui::Sizing::stretch()}}});
}

[[nodiscard]] ui::Node detailPane(PasteController& controller) {
    return ui::panel(
        {.title = [&controller] { return controller.detailTitle(); },
         .padding = 1,
         .child = ui::column(
             {.children = {ui::forEach<PasteFact>(
                               [&controller] { return controller.facts(); },
                               [](PasteFact const& fact) { return ui::Key{fact.key}; },
                               [](::morph::reactive::Signal<PasteFact> const& fact) {
                                   return ui::text({.text = [&fact] { return fact.get().line; }});
                               }),
                           ui::scroll({.child = ui::text({.text = [&controller] { return controller.content(); }}),
                                       .common = {.layout = {.height = ui::Sizing::stretch()}}}),
                           ui::button({.label = "Delete this paste",
                                       .onClick = [&controller] { controller.remove(); },
                                       .common = {.enabled = [&controller] { return controller.canDelete(); }}})},
              .gap = 1}),
         .common = {.layout = {.width = ui::Sizing::stretch()}}});
}

}  // namespace

::morph::ui::Node pasteScreen(PasteController& controller) {
    return ui::column(
        {.children = {statusLine(controller),
                      ui::row({.children = {listPane(controller), detailPane(controller)},
                               .gap = 2,
                               .common = {.layout = {.height = ui::Sizing::stretch()}}})},
         .gap = 1});
}

}  // namespace pastebin::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_pastebin_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/pastebin/ladder_pastebin_tests "[pastebin][view]"
```

Expected: PASS. Mutation check: in `pasteTable`'s `.onSelectionChange`, add
`controller.open(*pasteIdOf(keys.front()));`
after the `highlight(...)` call. Expected FAIL in "pasteScreen: moving the selection opens nothing, activating a
row opens it once" (`CHECK_FALSE(client.controller.current().has_value())`, then `reads: 1` never appears).
Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/pastebin/app/views examples/pastebin/tests/test_paste_view.cpp
git commit -m "wip(pastebin): pasteScreen, the one screen as a view tree

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: The application, the one binary, and the old client deleted

**Files:**
- Create: `examples/pastebin/app/pastebin_application.hpp`, `examples/pastebin/app/pastebin_application.cpp`
- Create: `examples/pastebin/ui/main.cpp`
- Modify: `examples/pastebin/CMakeLists.txt` — the WebAssembly server url block
- Modify: `.github/workflows/wasm-ladder.yml` — the named-target build step (Part 4 already configures
  `MORPH_BUILD_QT_QUICK` and builds `morph_qt_quick` there)
- Modify: `.github/workflows/ci.yml` — the `ladder-tests` Configure step
- Modify: `scripts/coverage.sh` — the rung source loop (`for _sub in include src gui_lib`) and its comment
- Modify: `codecov.yml` — pastebin's component comment and its two `ignore:` entries
- Delete: `examples/pastebin/gui/`, `examples/pastebin/gui_lib/`, `examples/pastebin/gui_wasm/`,
  `examples/pastebin/tests/test_paste_presenter.cpp`, `test_paste_qml_bridges.cpp`, `test_gui_qml_smoke.cpp`
- Test: `examples/pastebin/tests/smoke/test_pastebin_frontends.cpp` (binary `ladder_pastebin_smoke_tests`, C3)

**Interfaces:**
- Consumes: `ui::{Application, AppContext, ApplicationFactory, FrontendOption, selectFrontend}`
  (`morph/ui/frontend.hpp`); `tui::frontendOption(FrontendConfig = {})` (`morph/tui/frontend.hpp`, Part 3);
  `qt_quick::frontendOption(int&, char**, ui::EnvironmentReader = ui::processEnvironment())`
  (`morph/qt_quick/frontend.hpp`, Part 4); `examples::{AppEnvironment::fromArgs, connect, Connection::bridge,
  Connection::callbacks, LocalSetup}`; `examples::testing::{runFrontendSmoke, SmokeFrontend}`;
  `morph::ladder::testkit::DbFixture::computeConnectionString` (public, `examples/common/testkit/db_fixture.hpp`);
  `pastebin::db::setup` (`examples/pastebin/include/pastebin/db/database.hpp`).
- Produces: `pastebin::client::PastebinApplication(ui::AppContext&, examples::AppEnvironment const&)`,
  `pastebin::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&) ->
  std::unique_ptr<ui::Application>` (the contract's convention: the local database setup — `env.db`, else
  `PASTEBIN_DB`, else `pastebin.db` — is built inside); the `pastebin` binary.

Claims of the deleted test files, and where each one now lives:

| Deleted case | Re-expressed as |
|---|---|
| presenter: create then get round-trips (3 modes) | `test_paste_controller.cpp` "The create form stores pastes…" + "PasteController::open shows every fact…" |
| presenter: edit replaces content and syntax (3 modes) | dropped — no screen offers `EditPaste` (it never had one); the model behaviour is `test_paste_model.cpp` "EditPaste replaces an editable paste's content and syntax" |
| presenter: remove, then get fails (3 modes) | "PasteController::remove deletes the open paste, and opening it again fails" |
| presenter: list returns the pastes just created (3 modes) | "The create form stores pastes, lists them…" |
| presenter: every action routes its failure | create: "The create form is not ready while CreatePaste::validate refuses…" (`validate()` gates the typed form, so a refused body never reaches the wire); get: "An unknown id reports the model's message…"; delete: same case (`remove()` with nothing open issues nothing — the only way the UI can send a disengaged id); list: "A failed listing reports its error…" |
| presenter: get unknown id emits failed | "An unknown id reports the model's message…" |
| bridges: the QML surface audit | dropped with the QML (spec 4 §7) |
| bridges: a successful create relayed (3 modes) | "The create form stores pastes…" (`statusText() == "created <id>"`) |
| bridges: a rejected create relayed, nothing stored | "The create form is not ready while CreatePaste::validate refuses, and submitting it sends nothing" |
| bridges: destroyed with a submit in flight | "a controller destroyed with a create in flight drops its reply" |
| bridges: the open bag carries every key (3 modes) | "PasteController::open shows every fact…" |
| bridges: rows in the narrower shape, public only | "PasteController lists the public pastes in the narrower row shape" (+ `static_assert(!CarriesContent<PasteRow>)`) |
| bridges: the engaged arm of every formatted field | "PasteController renders the engaged arm of every formatted fact" |
| bridges: remove, then open fails with the model's message (3 modes) | "PasteController::remove deletes the open paste…" |
| bridges: open unknown id emits failed, not loaded | "An unknown id reports the model's message…" |
| smoke: Main.qml loads; the create form renders its Submit | `tests/smoke/test_pastebin_frontends.cpp` (TUI and Qt Quick) and `test_paste_view.cpp` ("Create paste" button) |

- [ ] **Step 1: Write the failing test**

Create `examples/pastebin/tests/smoke/test_pastebin_frontends.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"
#include "pastebin_application.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/frontend_smoke.hpp"

#if MORPH_EXAMPLE_HAS_TUI || MORPH_EXAMPLE_HAS_QT_QUICK

namespace {

/// @brief The application in local mode, `--db` naming the database `DbFixture` already prepared: the
///        application's own setup points at it again and finds every migration applied.
[[nodiscard]] morph::ui::ApplicationFactory pastebinFactory() {
    return [](morph::ui::AppContext& ctx) {
        morph::examples::AppEnvironment env;
        env.db = morph::ladder::testkit::DbFixture::computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"));
        return pastebin::client::makeApplication(ctx, env);
    };
}

}  // namespace

#endif

#if MORPH_EXAMPLE_HAS_TUI
TEST_CASE("pastebin mounts and quits on the TUI", "[pastebin][frontend]") {
    morph::ladder::testkit::DbFixture const fixture;
    morph::examples::testing::runFrontendSmoke(pastebinFactory(), morph::examples::testing::SmokeFrontend::Tui);
}
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
TEST_CASE("pastebin mounts and quits on Qt Quick", "[pastebin][frontend]") {
    morph::ladder::testkit::DbFixture const fixture;
    morph::examples::testing::runFrontendSmoke(pastebinFactory(), morph::examples::testing::SmokeFrontend::QtQuick);
}
#endif
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_pastebin_smoke_tests`
Expected: FAIL — `'pastebin_application.hpp' file not found`.

- [ ] **Step 3: Implement the application and the binary**

Create `examples/pastebin/app/pastebin_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>

#include "app/app_environment.hpp"
#include "app/transport.hpp"
#include "controllers/paste_controller.hpp"

/// @file
/// pastebin as a `ui::Application`: one connection, one controller, one screen.

namespace pastebin::client {

/// @brief pastebin's application. Members die controller first, so every handler is gone before the
///        bridge it registered with.
class PastebinApplication final : public ::morph::ui::Application {
public:
    /// @param ctx The frontend's context: the runtime, and the executor replies are delivered on.
    /// @param env What the command line (or the browser build) asked for; without `--server` its `db` (else
    ///            `PASTEBIN_DB`, else `pastebin.db` in the working directory) is the database set up and hosted.
    PastebinApplication(::morph::ui::AppContext& ctx, ::morph::examples::AppEnvironment const& env);

    ~PastebinApplication() override = default;
    PastebinApplication(PastebinApplication const&) = delete;
    PastebinApplication& operator=(PastebinApplication const&) = delete;
    PastebinApplication(PastebinApplication&&) = delete;
    PastebinApplication& operator=(PastebinApplication&&) = delete;

    /// @brief The screen.
    /// @return `pasteScreen()` over the controller.
    [[nodiscard]] ::morph::ui::Node view() override;

private:
    std::unique_ptr<::morph::examples::Connection> _connection;
    PasteController _controller;
};

/// @brief The application factory `ui/main.cpp` and the smoke tests share.
/// @param ctx The frontend's context.
/// @param env The environment.
/// @return The application.
[[nodiscard]] std::unique_ptr<::morph::ui::Application> makeApplication(::morph::ui::AppContext& ctx,
                                                                       ::morph::examples::AppEnvironment const& env);

}  // namespace pastebin::client
```

Create `examples/pastebin/app/pastebin_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "pastebin_application.hpp"

#include <cstdlib>
#include <string>

#include "views/paste_view.hpp"

#ifndef __EMSCRIPTEN__
#include "pastebin/db/database.hpp"
#endif

namespace pastebin::client {

namespace {

/// @brief How a client without `--server` prepares its database: `--db`, else `PASTEBIN_DB`, else `pastebin.db`
///        in the working directory. A remote client never calls it, and the browser build is always remote.
[[nodiscard]] ::morph::examples::LocalSetup localSetup() {
#ifdef __EMSCRIPTEN__
    return ::morph::examples::LocalSetup{};
#else
    return ::morph::examples::LocalSetup{.setupDatabase = [](std::string const& database) {
        if (!database.empty()) {
            db::setup(database);
            return;
        }
        char const* fromEnvironment = std::getenv("PASTEBIN_DB");
        db::setup(fromEnvironment != nullptr ? fromEnvironment : "DRIVER=SQLite3;Database=pastebin.db;Timeout=5000");
    }};
#endif
}

}  // namespace

PastebinApplication::PastebinApplication(::morph::ui::AppContext& ctx, ::morph::examples::AppEnvironment const& env)
    : _connection{::morph::examples::connect(ctx, env, localSetup())},
      _controller{ctx.runtime(), _connection->bridge(), _connection->callbacks()} {}

::morph::ui::Node PastebinApplication::view() { return pasteScreen(_controller); }

std::unique_ptr<::morph::ui::Application> makeApplication(::morph::ui::AppContext& ctx,
                                                          ::morph::examples::AppEnvironment const& env) {
    return std::make_unique<PastebinApplication>(ctx, env);
}

}  // namespace pastebin::client
```

Create `examples/pastebin/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

/// @file
/// pastebin's one binary, native and WebAssembly: it reads the environment, offers every frontend this
/// build has, lets `ui::selectFrontend` pick one (`--ui=`, `MORPH_UI`, else the first usable), and hands the
/// frontend the application factory.
///
/// @code
/// pastebin                                  # in-process: hosts PasteModel over --db / PASTEBIN_DB
/// pastebin --server ws://127.0.0.1:8765     # against ladder_pastebin_server
/// pastebin --ui=tui                         # force the terminal UI
/// @endcode
///
/// The browser build has no argv to read a server from: it is always remote, against the url baked in as
/// `MORPH_LADDER_PASTEBIN_WASM_SERVER_URL` (a `?server=` in the page url wins), and it never touches a database.

#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <string>
#include <vector>

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#endif
#if MORPH_EXAMPLE_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

#include "app/app_environment.hpp"
#include "pastebin_application.hpp"

int main(int argc, char** argv) {
    try {
        auto env = morph::examples::AppEnvironment::fromArgs(argc, argv);
#ifdef __EMSCRIPTEN__
        if (!env.server.has_value()) {
            env.server = std::string{MORPH_LADDER_PASTEBIN_WASM_SERVER_URL};
        }
#endif
        std::vector<morph::ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
        built.push_back(morph::qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
        built.push_back(morph::tui::frontendOption());
#endif
        auto const frontend = morph::ui::selectFrontend(built, argc, argv);
        return frontend->run(
            [&env](morph::ui::AppContext& ctx) { return pastebin::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "pastebin: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "pastebin: unknown error\n";
    }
    return 1;
}
```

Replace the WebAssembly block of `examples/pastebin/CMakeLists.txt` (everything after `morph_add_rung(NAME pastebin)`)
with:

```cmake
# ── The browser build's server url ──────────────────────────────────────────
# A page served from a static bundle has no argv to read --server from, so the url the browser build of
# ui/main.cpp connects to is a build-time constant, per rung so several rungs' clients can point at their own
# servers in one Emscripten configure.
if(EMSCRIPTEN AND TARGET pastebin)
    if(NOT DEFINED MORPH_LADDER_PASTEBIN_WASM_SERVER_URL)
        set(MORPH_LADDER_PASTEBIN_WASM_SERVER_URL "ws://127.0.0.1:8765" CACHE STRING
            "URL pastebin's browser build connects to; must be a reachable ladder_pastebin_server.")
    endif()
    target_compile_definitions(pastebin PRIVATE
        MORPH_LADDER_PASTEBIN_WASM_SERVER_URL="${MORPH_LADDER_PASTEBIN_WASM_SERVER_URL}"
    )
endif()
```

- [ ] **Step 4: Delete the old client and its tests**

```bash
git rm -r -q examples/pastebin/gui examples/pastebin/gui_lib examples/pastebin/gui_wasm \
    examples/pastebin/tests/test_paste_presenter.cpp examples/pastebin/tests/test_paste_qml_bridges.cpp \
    examples/pastebin/tests/test_gui_qml_smoke.cpp
```

- [ ] **Step 5: CI, coverage and the WebAssembly gate**

`.github/workflows/wasm-ladder.yml`'s Configure step already carries `-DMORPH_BUILD_QT_QUICK=ON` (Part 4); check
with `grep -n 'MORPH_BUILD_QT_QUICK' .github/workflows/wasm-ladder.yml` and add nothing. In the step "Build the
WASM-remote spike and every rung's WASM client", replace the loop body

```bash
            if [ -d "examples/$rung/gui_wasm" ]; then
              cmake --build build-wasm-ladder --target "ladder_${rung}_gui_wasm"
              built=$((built + 1))
            else
              echo "::notice::examples/$rung/gui_wasm not present in this checkout -- skipping ladder_${rung}_gui_wasm (see wasm-ladder.yml's comment)"
            fi
```

with

```bash
            if [ -f "examples/$rung/ui/main.cpp" ]; then
              cmake --build build-wasm-ladder --target "$rung"
              built=$((built + 1))
            elif [ -d "examples/$rung/gui_wasm" ]; then
              cmake --build build-wasm-ladder --target "ladder_${rung}_gui_wasm"
              built=$((built + 1))
            else
              echo "::notice::examples/$rung has neither ui/main.cpp nor gui_wasm/ -- no browser client to build (see wasm-ladder.yml's comment)"
            fi
```

and the error line's text `no ladder_<rung>_gui_wasm target was built` with `no rung's browser client was built`.
In that step's leading comment, replace "every rung that has a gui_wasm/ directory" with "every rung that has a
browser client — `ui/main.cpp` (target `<rung>`, built with Qt Quick only) or, before it migrates, `gui_wasm/`
(target `ladder_<rung>_gui_wasm`)".

In `.github/workflows/ci.yml`, first run `grep -n 'MORPH_BUILD_TUI\|MORPH_BUILD_QT_QUICK' .github/workflows/ci.yml`:
Part 3 adds `-DMORPH_BUILD_TUI=ON` to `linux-sanitizers`, `linux-all-features` and `clang-tidy`, and Part 4 adds
`-DMORPH_BUILD_QT_QUICK=ON` to `linux-coverage`, `ladder-tests`, `ladder-sanitizers`, `linux-all-features` and
`clang-tidy`. The one line missing for this plan is in the `ladder-tests` job's "Configure (gcc-debug, ladder + Qt +
bank on)" step: add `-DMORPH_BUILD_TUI=ON \` directly after `-DMORPH_BUILD_QT_QUICK=ON \` there. Without it a
rung's terminal smoke case is skipped in CI (spec 4 §6). Edit no other job.

In `scripts/coverage.sh`, change `for _sub in include src gui_lib; do` to `for _sub in include src gui_lib app; do`
and replace the comment above that loop's `# Only rungs whose test binary…` paragraph — the lines from
`# include/ + src/ are each rung's DTOs` through `# exercised only by the offscreen QML smoke test and by hand.`
— with:

```bash
# include/ + src/ are each rung's DTOs and models (rule 5's own 100% bar);
# app/ is a migrated rung's controllers and views, and gui_lib/ an unmigrated
# rung's presenters and QML adapters — both held to the same bar, since both
# are real coverage of morph's client stack. A rung's main() shells (ui/, and
# gui/ and gui_wasm/ before it migrates) are deliberately absent: they read
# argv and pick a frontend, with no unit-testable seam, and are exercised by
# the frontend smoke tests and by hand.
```

In `codecov.yml`, replace the two `ignore:` entries `"examples/pastebin/gui/**"` and
`"examples/pastebin/gui_wasm/**"` with the one entry `"examples/pastebin/ui/**"`, and replace the pastebin
component's comment — from `    # Rung 1, pastebin.` down to the line before `    - component_id: pastebin` — with:

```yaml
    # Rung 1, pastebin.
    #
    # What is actually measured, precisely — the `paths` glob below is
    # `examples/pastebin/**`, but a component can only score files the
    # uploaded report contains, and that report is whatever
    # `scripts/coverage.sh` names in its `SOURCES` array. For this rung that
    # is `include/`, `src/` and `app/`: the DTOs, the model, the server's
    # App, and the client's controller and views. It is **not** `ui/` — the
    # `main()` shell (argv, frontend selection) has no unit-testable seam,
    # is exercised by the frontend smoke tests and by hand, and is named in
    # `ignore:` below so its absence is a decision rather than an accident.
    # `tests/` is excluded for the same reason examples/common's is: a suite
    # scoring its own test code inflates the number it is supposed to police.
    #
    # Every line `llvm-cov report` finds missed in `include/` and `src/` is
    # accounted for:
    #   * units.hpp (2) — the `default:` arm of `UnitTraits<Unit>::meta`'s
    #     switch. `Unit` has exactly one enumerator, so that arm is
    #     unreachable without undefined behavior; it exists because the
    #     repo's warning policy requires a switch default.
    #   * src/server/app/app.cpp (4) — `sweepExpiredOnce()`'s `.onError`
    #     branch, which logs an `ExpirePaste` that failed to dispatch.
    #     Provoking a dispatch failure through a `SimulatedRemoteBackend`
    #     needs a fault-injection seam this rung's tests do not use.
    #   * src/models/paste_model.cpp (2 or 3) — the `rows.empty()` guard in
    #     `execute(GetPaste)`'s read-back, unreachable in practice, plus one
    #     line that depends on which classifier branch the two
    #     `DbBusyFixture` store-error cases land in: they race a real SQLite
    #     lock.
    # `app/` is measured by `tests/test_paste_controller.cpp`,
    # `tests/test_paste_view.cpp` and `tests/smoke/test_pastebin_frontends.cpp`.
    #
    # The target is informational and sits below the measured ceiling, so the
    # run-to-run line above cannot flip it.
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build/all --target ladder_pastebin_tests ladder_pastebin_smoke_tests pastebin ladder_pastebin_server
QT_QPA_PLATFORM=offscreen ./build/all/examples/pastebin/ladder_pastebin_tests "[pastebin]"
ctest --test-dir build/all -R '^pastebin\.smoke\.' --output-on-failure
ls examples/pastebin    # CMakeLists.txt README.md app include src tests ui
```

Expected: PASS; the `ctest` line runs both frontend cases (this configure builds both frontends). Mutation
check: change `PastebinApplication::view()`'s body to `throw std::logic_error{"no view"};` (add
`#include <stdexcept>`). Expected FAIL in both `pastebin.smoke.` cases — the frontend cannot mount, so its run does
not end with exit code 0. Restore.

- [ ] **Step 7: Commit**

```bash
git add -A examples/pastebin .github/workflows/wasm-ladder.yml .github/workflows/ci.yml scripts/coverage.sh codecov.yml
git commit -m "wip(pastebin): the application, one binary for every frontend, the QML client removed

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: pastebin's README and the Getting Started walkthrough

**Files:**
- Modify: `examples/pastebin/README.md` — the intro, "Running it", the `CallbackScope` bullet, the paragraph after
  it, three Definition-of-done bullets, two Known-gaps bullets, the `../bank/gui_wasm` link
- Modify: `docs/GETTING-STARTED.md` — §3's commands and note, §9, §10's opening, §11's last paragraphs, §12's
  "nothing changes" and "what differs" passages

**Interfaces:** none.

- [ ] **Step 1: Check every path the two documents name still exists**

```bash
grep -n 'gui_lib\|gui_wasm\|ladder_pastebin_gui\|gui/main\.cpp\|gui/qml\|paste_qml_bridges\|paste_presenter\|paste_schemas\|test_gui_qml_smoke' \
    examples/pastebin/README.md docs/GETTING-STARTED.md
```

Expected before the edit: the references the steps below replace (README lines 30–35, 91, 212–238, 309–331,
400–406; GETTING-STARTED §3, §9, §10). After Step 3 the command prints nothing.

- [ ] **Step 2: Rewrite pastebin's README**

In the intro paragraph replace "one entity, one model, SQLite, Qt WASM client." with "one entity, one model,
SQLite, and one client that runs in a terminal or as a Qt Quick window, natively or in a browser."

Replace the "Running it" code block and the paragraph after it (up to, not including, "**Scope note**") with:

````markdown
```bash
# One-time configure (Qt 6.5+ and an ODBC SQLite3 driver; the client needs at
# least one frontend, the terminal UI or Qt Quick):
cmake -S . -B build -G Ninja \
    -DMORPH_BUILD_QT=ON -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON \
    -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=pastebin

# Server (owns the database, the action journal and the expiry sweep):
PASTEBIN_DB="DRIVER=SQLite3;Database=pastebin.db;Timeout=5000" \
PASTEBIN_PORT=8765 ./build/examples/pastebin/ladder_pastebin_server

# The client, either deployment mode, on whichever frontend suits:
./build/examples/pastebin/pastebin                                  # in-process (--db, else PASTEBIN_DB)
./build/examples/pastebin/pastebin --server ws://127.0.0.1:8765
./build/examples/pastebin/pastebin --ui=tui                         # force the terminal UI
```

`--ui=` (or `MORPH_UI`) picks the frontend; without either, a Qt Quick window opens where there is a display
and the terminal UI otherwise. The browser client is the same `ui/main.cpp` built in an Emscripten configure
with Qt Quick only — which additionally needs `-DMORPH_CLIENT_ONLY=ON`, since it names its model type but must
not link the model's ODBC-backed bodies (`docs/spec/core/registry.md`). It is always remote, against the url
baked in via `-DMORPH_LADDER_PASTEBIN_WASM_SERVER_URL=ws://host:port`. The configure line CI uses is
`.github/workflows/wasm-ladder.yml`.
````

Replace "(follow [`../bank/gui_wasm`](../bank/gui_wasm))" with "(follow [`../bank`](../bank))".

Replace the `**morph::async::CallbackScope**` bullet and the paragraph after it ("Pastebin's GUI renders
exclusively …" through "presenter rule requires.") with:

```markdown
- **`morph::async::CallbackScope`**, through `morph::reactive`: every reply the client waits for is delivered
  to a `Query` or `Mutation` (the create form's submission included) whose last-declared `CallbackScope`
  refuses it once the node is gone. `tests/test_paste_controller.cpp`'s "a controller destroyed with a create
  in flight drops its reply" drives the window (destroy the controller mid-submit, let the reply land) and
  asserts the observable half — the create still happened. The suppression itself has no observable signature
  outside a sanitized build, which the case states rather than dresses up.

The client is toolkit-free (`app/`, target `ladder_pastebin_app`): `PasteController` owns the listing (a
`Query<ListPastes>`), opening a paste (a `Mutation<GetPaste>` — never a query, because `GetPaste` consumes one
of the paste's reads and must not be refetched behind the user's back), deleting it, and the create form, a
typed `forms::Form<CreatePaste>` rendered by `forms::formView` from `schemaJson<CreatePaste>()`. Opening and
deleting both invalidate the listing, so a burn shows without a manual refresh. `views/paste_view.cpp` binds a
view tree to it and decides nothing; `ui/main.cpp` picks the frontend.
```

In "Definition of done", replace the bullet "**Desktop client against local and remote backends.**" (through
"smoke test in the suite.") with:

```markdown
- [x] **Client against local and remote backends.** `pastebin` in both modes, on the terminal UI and on Qt
  Quick, driven manually against a real `ladder_pastebin_server` (create → list → open → burn → delete), and
  by `tests/smoke/test_pastebin_frontends.cpp`, which mounts the application on each built frontend and quits.
```

replace the bullet "**WASM client, same client code.**" (through "the compile gate that will settle it.") with:

```markdown
- [~] **WASM client, same client code.** The browser client is `ui/main.cpp` itself, built with Qt Quick only:
  two `__EMSCRIPTEN__` branches — the server url in `ui/main.cpp`, no database setup in
  `app/pastebin_application.cpp` — and nothing else differs: no shadow headers, no WASM variant of any controller,
  view, model or DTO ([`../TESTING.md`](../TESTING.md)'s hard requirement). `.github/workflows/wasm-ladder.yml` builds target `pastebin` in its Emscripten configure;
  no Emscripten toolchain was available where this client was written, so that job is the first compile.
```

and replace the bullet "**Presenter-shaped GUI.**" (two lines) with:

```markdown
- [x] **Toolkit-free client.** `ladder_pastebin_app` links morph and `ladder_pastebin_lib` only, and the domain
  library links no Qt (the server's `App` lives in `ladder_pastebin_server_app`); `PasteController` is tested
  in all three backend modes, and the screen on `RecordingBackend`.
```

In "Known gaps", replace the bullet "**`ladder-tests` still builds no GUI.**" with:

```markdown
- **The browser client is compile-gated, not run.** `wasm-ladder.yml` builds it; nothing in CI loads it in a
  browser against a server.
```

and in the bullet "**Registration timing.**" replace "Both clients' `Main.qml` request the first listing on
`Component.onCompleted`." with "The controller requests the first listing when it is constructed." and "and the
list pane empty with no terminal error." with "and the listing on its busy indicator with no terminal error."

Add, directly before "### Known gaps, stated rather than smoothed over", a section:

```markdown
### What the client shows differently from the QML client it replaced

- The listing is a table (paste, syntax, visibility, created) rather than a one-line list. Selecting a row only
  highlights it; **activating** it (Enter, a double click) opens it — selecting used to open, and every open
  consumes a read.
- The create form is the forms engine's rendering of `CreatePaste`'s schema: the burn budget is a number field,
  the expiry a date-time field, visibility and editability drop-downs. Its Submit button reads "Create paste".
- The status line keeps the last open, delete or create outcome; it no longer clears on every listing.
- In a terminal the paste body is a scrolling text block; there is no text selection for copying.
```

- [ ] **Step 3: Rewrite the Getting Started walkthrough's pastebin sections**

In the replacements below and in Step 2's, a `\n` inside a quoted original marks a line break in the file.

In `docs/GETTING-STARTED.md`'s contents list replace `[9. A GUI on top](#9-a-gui-on-top)` with
`[9. A UI on top](#9-a-ui-on-top)`; in §3's first paragraph replace "a Qt/QML client" with "one client that runs
in a terminal or as a Qt Quick window"; in §4 replace "Nothing domain-shaped may live in a presenter, in QML, or
in `main()`." with "Nothing domain-shaped may live in a controller, in a view, or in `main()`."

In §3, replace the configure/build block with:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMORPH_BUILD_QT=ON -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON \
    -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=pastebin \
    -DMORPH_BUILD_TESTS=ON -DMORPH_BUILD_EXAMPLES=ON
cmake --build build --target ladder_pastebin_tests ladder_pastebin_server pastebin
```

replace "100% tests passed out of 55" with "100% tests passed", replace "The desktop client runs in either
deployment mode." and its two command blocks with:

````markdown
The client runs in either deployment mode, in a terminal or as a Qt Quick window. Against the server:

```sh
./build/examples/pastebin/pastebin --server ws://127.0.0.1:8765
```

or entirely in-process, hosting `PasteModel` itself:

```sh
PASTEBIN_DB="DRIVER=SQLite3;Database=local.db;Timeout=5000" \
  ./build/examples/pastebin/pastebin --ui=tui
```
````

and in the note below them replace "[`gui/main.cpp:53-73`](../examples/pastebin/gui/main.cpp) explains that" with
"[`app/pastebin_application.cpp`](../examples/pastebin/app/pastebin_application.cpp)'s `localSetup()` is the only
place a client touches a database, and".

Replace §9 ("## 9. A GUI on top" through the paragraph ending "calls `refresh()` straight away.") with:

````markdown
## 9. A UI on top

pastebin's client is three small layers, none of which names a toolkit.

**The controller** says what the screen depends on and what each write invalidates
([`app/controllers/paste_controller.cpp`](../examples/pastebin/app/controllers/paste_controller.cpp)):

```cpp
_list{runtime, _handler, [] { return std::optional<ListPastes>{ListPastes{}}; }},
_open{runtime, _handler, MutationOptions{.invalidates = {&_list}}},
_delete{runtime, _handler, MutationOptions{.invalidates = {&_list}}},
```

The listing is a `Query`: fetched on construction, kept while a refetch is in flight, its stale replies
dropped. Opening is a `Mutation`, not a query, and that is a domain decision: `GetPaste` consumes one of the
paste's reads, so it may only be issued when the user asks — never refetched. Each success invalidates the
listing, which is how a burned paste disappears without anyone calling `refresh()`. Formatting lives here too,
in `Computed`s (`rows()`, `facts()`), so it is tested without a screen.

**The view** is bindings only
([`app/views/paste_view.cpp`](../examples/pastebin/app/views/paste_view.cpp)):

```cpp
ui::button({.label = "Delete this paste",
            .onClick = [&controller] { controller.remove(); },
            .common = {.enabled = [&controller] { return controller.canDelete(); }}})
```

A lambda is a binding: the button re-enables itself when `canDelete()` changes, and nothing else is redrawn.

**The composition root** picks the frontend and the transport
([`ui/main.cpp`](../examples/pastebin/ui/main.cpp)):

```cpp
auto const frontend = morph::ui::selectFrontend(built, argc, argv);
return frontend->run(
    [&env](morph::ui::AppContext& ctx) { return pastebin::client::makeApplication(ctx, env); });
```

`makeApplication` builds the bridge inside the frontend's context (`examples::connect`: a `LocalBackend` over the
database `--db` names, or the frontend's socket transport for `--server`) and constructs the controller over it. A handler's
registration round trip settles asynchronously, and nothing has to wait for it: a call made through a handler
whose bind is still in flight is held and dispatched when the bind settles, so the listing query fires straight
away.
````

In §10 replace the two lines "pastebin's create form renders from one line" and
"([`gui_lib/paste_schemas.hpp:31`](…)):", and the code block after them, with:

````markdown
pastebin's create form is one member of its controller:

```cpp
::morph::forms::Form<CreatePaste, PasteModel> _create;
```

`Form<A>` builds its field model from `schemaJson<A>()`, and `forms::formView` renders it on whichever frontend
is running.
````

In §11 append, after the paragraph ending "settles. Section 12 shows what that combination is for.":

```markdown
The client is tested the same way, one layer up: a `PasteController` over a `BackendRig` client, its reactive
runtime on the rig's executor, asserted through its accessors
([`tests/test_paste_controller.cpp`](../examples/pastebin/tests/test_paste_controller.cpp)); the screen is
mounted on `ui::testing::RecordingBackend`, a headless widget tree a test can click
([`tests/test_paste_view.cpp`](../examples/pastebin/tests/test_paste_view.cpp)).
```

In §12 replace "the same\n`PasteBridge`, the same `PastePresenter`, the same `BridgeHandler<PasteModel>`," with
"the same\n`PasteController`, the same `BridgeHandler<PasteModel>`," and replace "Build\n  your handlers inside a
readiness callback (section 9); a first fetch made\n  before the handler's bind settles is held until it does."
with "A first\n  fetch made before the handler's bind settles is held until it does (section 9)."

- [ ] **Step 4: Verify**

Re-run Step 1's `grep`. Expected: no output. Then
`npx markdownlint-cli2 examples/pastebin/README.md docs/GETTING-STARTED.md`
(or `pre-commit run markdownlint --files …`). Expected: no findings.

- [ ] **Step 5: Commit**

```bash
git add examples/pastebin/README.md docs/GETTING-STARTED.md
git commit -m "wip(pastebin): README and Getting Started describe the new client

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Verify pastebin, and squash its group

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: The rung's whole suite, and the shared testkit's**

```bash
cmake --build build/all
ctest --test-dir build/all -L 'ladder-pastebin|ladder-0' --output-on-failure
```

Expected: every test passes.

- [ ] **Step 2: Prove the client is toolkit-free from the build's own command lines**

```bash
grep -rnE '#include <Q|morph/(qt|tui|qt_quick)/' examples/pastebin/app && echo "toolkit include in app/" || true
for t in ladder_pastebin_app ladder_pastebin_lib; do
  own=$(ninja -C build/all -t commands "$t" | grep "CMakeFiles/$t.dir/" || true)
  m=$(printf '%s\n' "$own" | grep -c . || true)
  n=$(printf '%s\n' "$own" | grep -cE 'Qt6|QtCore|/qt/' || true)
  echo "$t: $m own compile lines, $n Qt-bearing"; test "$m" -gt 0 && test "$n" -eq 0
done
```

Expected: the `grep` prints nothing, and both targets report a non-zero count of their own compile lines and `0
Qt-bearing` — while the same pipeline over `ladder_pastebin_server_app` reports a non-zero Qt-bearing count (the
check can see Qt where it is). Only the target's own objects are counted (`CMakeFiles/<target>.dir/`): `ninja -t
commands` also lists its dependencies' commands, and a dependency that compiles Qt glue is not the claim here.

- [ ] **Step 3: The model tests are untouched**

```bash
git diff --exit-code master -- examples/pastebin/tests/test_paste_model.cpp examples/pastebin/tests/.clang-tidy
```

Expected: exit 0.

- [ ] **Step 4: Run the application by hand**

```bash
PASTEBIN_PORT=8765 ./build/all/examples/pastebin/ladder_pastebin_server --seed &
./build/all/examples/pastebin/pastebin --server ws://127.0.0.1:8765 --ui=tui
./build/all/examples/pastebin/pastebin --server ws://127.0.0.1:8765 --ui=qt
PASTEBIN_DB="DRIVER=SQLite3;Database=/tmp/pastebin-local.db;Timeout=5000" ./build/all/examples/pastebin/pastebin --ui=tui
kill %1
```

On each: create a paste, see it listed, open the seeded burn-after-1 paste and watch it leave the listing,
delete a paste. Record which frontend/mode combinations were exercised in the squash body; this is the step a
unit test cannot replace.

- [ ] **Step 5: Sanitizer** (Linux; on macOS an ASan configure of the same tree)

```bash
cmake --preset clang-asan -DMORPH_BUILD_QT=ON -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON \
      -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=pastebin
cmake --build build/clang-asan --target ladder_pastebin_tests
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/examples/pastebin/ladder_pastebin_tests asan
QT_QPA_PLATFORM=offscreen ./build/clang-asan/examples/pastebin/ladder_pastebin_tests "[pastebin][controller],[pastebin][view]"
```

Expected: the instrumentation check passes and the run is clean; then perform Task 4's second mutation check
here and see it fail, and restore.

- [ ] **Step 6: clang-tidy over the changed lines** — CONTRIBUTING's recipe with `origin/master...HEAD`, the file
  count printed and asserted non-zero. Expected: no findings in `examples/pastebin/app/`, `ui/`, `tests/`,
  `examples/common/app/form_success.hpp`.

- [ ] **Step 7: Commit any fixes**

```bash
git add -A examples/pastebin examples/common cmake
git commit -m "wip(pastebin): fixes from the verification gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if nothing needed fixing, and say so in the hand-off.

- [ ] **Step 8: Squash the group**

Follow the master plan's "Squashing a part" procedure with `key=pastebin` and this message (add a line for any
deviation from this plan, per the master plan's "The docs commit"):

```text
examples/pastebin: one app, any frontend

pastebin's client is a toolkit-free app library — PasteController (a
ListPastes query; an explicit GetPaste mutation, never refetched because a
read consumes the paste's budget; a delete; a typed Form<CreatePaste>) and
its view — plus one binary, ui/main.cpp, on the TUI or Qt Quick, native or
in the browser. Its domain library no longer links Qt: morph_add_rung gains
ladder_<rung>_server_app for a server's QObject App. examples/common gains
FormSuccess. The QML client and its tests are gone, their claims re-expressed.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure must show the history so far ending in `examples/pastebin: one app, any frontend`.

---
## Group 2 — bookmarks (key `bookmarks`)

### Task 9: bookmarks' `App` into the server, display helpers, and the browser build's sources

**Files:**
- Move: `examples/bookmarks/include/bookmarks/app/{app,metadata_fetcher}.hpp` →
  `examples/bookmarks/src/server/include/bookmarks/app/`
- Move: `examples/bookmarks/src/app/app.cpp` → `examples/bookmarks/src/server/app/app.cpp`
- Modify: `examples/bookmarks/CMakeLists.txt` — whole file
- Create: `examples/bookmarks/app/controllers/bookmark_text.hpp`,
  `examples/bookmarks/app/controllers/bookmark_text.cpp`
- Test: `examples/bookmarks/tests/test_bookmark_text.cpp`; the rung's existing `tests/test_app.cpp` (it constructs
  `bookmarks::app::App`)

**Interfaces:**
- Consumes: Task 1's `ladder_<rung>_server_app` convention; bookmarks' DTOs (`bookmarks/dto/*.hpp`), `Count`
  (`bookmarks/units.hpp`); `morph::units::toString`; `reactive::errorMessage`.
- Produces: `bookmarks::client::{StatusLine, isoOrEmpty, countText, visibilityText, readStateText,
  archiveStateText, rowTitle, headline, joined, failureLine, infoLine}`.

- [ ] **Step 1: Write the failing test**

Create `examples/bookmarks/tests/test_bookmark_text.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "bookmarks/units.hpp"
#include "controllers/bookmark_text.hpp"

using namespace bookmarks::client;

TEST_CASE("bookmark_text renders every enum arm and both title shapes", "[bookmarks][controller]") {
    CHECK(visibilityText(bookmarks::Visibility::Private) == "Private");
    CHECK(visibilityText(bookmarks::Visibility::Shared) == "Shared");
    CHECK(readStateText(bookmarks::ReadState::Unread) == "Unread");
    CHECK(readStateText(bookmarks::ReadState::Read) == "Read");
    CHECK(archiveStateText(bookmarks::ArchiveState::Active) == "Active");
    CHECK(archiveStateText(bookmarks::ArchiveState::Archived) == "Archived");
    CHECK(rowTitle("", "https://a.example") == "https://a.example");
    CHECK(rowTitle("A", "https://a.example") == "A · https://a.example");
    CHECK(headline("", "https://a.example") == "https://a.example");
    CHECK(headline("A", "https://a.example") == "A");
    CHECK(joined({"work", "home"}, ", ") == "work, home");
    CHECK(joined({}, ", ").empty());
    CHECK(countText(bookmarks::Count::fromDouble(2.0)) == "2");
    CHECK(isoOrEmpty(::morph::time::Timestamp{}).empty());
    CHECK(failureLine(std::make_exception_ptr(std::runtime_error{"no"})) == StatusLine{.text = "no", .isError = true});
    CHECK(infoLine("ok") == StatusLine{.text = "ok", .isError = false});
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_bookmarks_tests`
Expected: FAIL — `'controllers/bookmark_text.hpp' file not found`.

- [ ] **Step 3: Move the `App`, rewrite the rung's CMake, add the helpers**

```bash
mkdir -p examples/bookmarks/src/server/include/bookmarks/app examples/bookmarks/src/server/app
git mv examples/bookmarks/include/bookmarks/app/app.hpp examples/bookmarks/src/server/include/bookmarks/app/app.hpp
git mv examples/bookmarks/include/bookmarks/app/metadata_fetcher.hpp \
       examples/bookmarks/src/server/include/bookmarks/app/metadata_fetcher.hpp
git mv examples/bookmarks/src/app/app.cpp examples/bookmarks/src/server/app/app.cpp
```

In the moved `app.hpp` replace

```cpp
/// metadata-fetch worker, and the periodic outbox relay. Nothing here decides
/// deployment mode — that stays `examples/common/gui::AppContext`'s job on
/// the client side; this is exclusively the server side.
```

with

```cpp
/// metadata-fetch worker, and the periodic outbox relay. Nothing here decides
/// deployment mode — that is the client's (`examples::connect`); this is
/// exclusively the server side, built into `ladder_bookmarks_server_app`.
```

Replace `examples/bookmarks/CMakeLists.txt` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# bookmarks — rung 2 of the application ladder (examples/bookmarks/README.md).
# morph_add_rung() wires the standard targets (cmake/morph_add_rung.cmake); this
# file adds the sources its directory conventions do not cover, then the
# browser build's server url.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME bookmarks)

# The Netscape importer (src/import/) and Login's validation (src/dto/) sit
# outside the src/models, src/db and src/app globs.
if(TARGET ladder_bookmarks_lib)
    target_sources(ladder_bookmarks_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/import/netscape_bookmarks.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# The browser build has no ladder_bookmarks_lib (no ODBC in a browser), yet its
# client still names Login, whose validate() lives in src/dto/auth_dto.cpp — a
# file with no persistence dependency, so the client library compiles it there.
if(TARGET ladder_bookmarks_app AND NOT TARGET ladder_bookmarks_lib)
    target_sources(ladder_bookmarks_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# ── The browser build's server url ──────────────────────────────────────────
# A page served from a static bundle has no argv to read --server from.
if(EMSCRIPTEN AND TARGET bookmarks)
    if(NOT DEFINED MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL)
        set(MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL "ws://127.0.0.1:8766" CACHE STRING
            "URL bookmarks' browser build connects to; must be a reachable ladder_bookmarks_server.")
    endif()
    target_compile_definitions(bookmarks PRIVATE
        MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL="${MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL}"
    )
endif()
```

Create `examples/bookmarks/app/controllers/bookmark_text.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/units.hpp"

/// @file
/// The text every bookmarks controller shows: one place for each word and format, so the panes agree.

namespace bookmarks::client {

/// @brief A pane's status line: the last outcome it reports.
struct StatusLine {
    std::string text;      ///< Empty when there is nothing to report.
    bool isError = false;  ///< Whether `text` reports a failure.

    /// @brief Field-wise equality.
    bool operator==(StatusLine const&) const = default;
};

/// @brief An instant as ISO-8601, or empty when unset.
/// @param instant The instant.
/// @return Its text.
[[nodiscard]] std::string isoOrEmpty(::morph::time::Timestamp const& instant);

/// @brief A count as text (`morph::units::toString`).
/// @param count The count.
/// @return Its text.
[[nodiscard]] std::string countText(Count const& count);

/// @brief "Private" or "Shared".
/// @param visibility The visibility.
/// @return Its word.
[[nodiscard]] std::string visibilityText(Visibility visibility);

/// @brief "Unread" or "Read".
/// @param state The read state.
/// @return Its word.
[[nodiscard]] std::string readStateText(ReadState state);

/// @brief "Active" or "Archived".
/// @param state The archive state.
/// @return Its word.
[[nodiscard]] std::string archiveStateText(ArchiveState state);

/// @brief A listing row's title: "title · url", or the url alone while the title is unknown.
/// @param title The bookmark's title.
/// @param url Its url.
/// @return The row title.
[[nodiscard]] std::string rowTitle(std::string const& title, std::string const& url);

/// @brief A detail pane's headline: the title, or the url while the title is unknown.
/// @param title The bookmark's title.
/// @param url Its url.
/// @return The headline.
[[nodiscard]] std::string headline(std::string const& title, std::string const& url);

/// @brief @p parts joined by @p separator.
/// @param parts The parts.
/// @param separator Between two parts.
/// @return The joined text.
[[nodiscard]] std::string joined(std::vector<std::string> const& parts, std::string_view separator);

/// @brief A failure as a status line.
/// @param error The failure; must not be null.
/// @return `errorMessage(error)`, marked as an error.
[[nodiscard]] StatusLine failureLine(std::exception_ptr const& error);

/// @brief An outcome as a status line.
/// @param text What happened.
/// @return @p text, not an error.
[[nodiscard]] StatusLine infoLine(std::string text);

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/bookmark_text.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/bookmark_text.hpp"

#include <morph/reactive/control.hpp>
#include <morph/util/quantity.hpp>
#include <utility>

namespace bookmarks::client {

std::string isoOrEmpty(::morph::time::Timestamp const& instant) {
    return instant.hasValue() ? (*instant).toIso8601() : std::string{};
}

std::string countText(Count const& count) { return ::morph::units::toString(count); }

std::string visibilityText(Visibility visibility) { return visibility == Visibility::Shared ? "Shared" : "Private"; }

std::string readStateText(ReadState state) { return state == ReadState::Read ? "Read" : "Unread"; }

std::string archiveStateText(ArchiveState state) { return state == ArchiveState::Archived ? "Archived" : "Active"; }

std::string rowTitle(std::string const& title, std::string const& url) {
    return title.empty() ? url : title + " · " + url;
}

std::string headline(std::string const& title, std::string const& url) { return title.empty() ? url : title; }

std::string joined(std::vector<std::string> const& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

StatusLine failureLine(std::exception_ptr const& error) {
    return StatusLine{.text = ::morph::reactive::errorMessage(error), .isError = true};
}

StatusLine infoLine(std::string text) { return StatusLine{.text = std::move(text), .isError = false}; }

}  // namespace bookmarks::client
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/all --target ladder_bookmarks_tests ladder_bookmarks_server
QT_QPA_PLATFORM=offscreen ./build/all/examples/bookmarks/ladder_bookmarks_tests "[bookmarks]"
```

Expected: PASS, including `test_app.cpp`'s cases (now linked through `ladder_bookmarks_server_app`). Mutation
check: in `rowTitle`, swap the ternary's arms. Expected FAIL in "bookmark_text renders every enum arm and both
title shapes". Restore.

- [ ] **Step 5: Commit**

```bash
git add -A examples/bookmarks/src examples/bookmarks/include examples/bookmarks/CMakeLists.txt \
    examples/bookmarks/app examples/bookmarks/tests/test_bookmark_text.cpp
git commit -m "wip(bookmarks): App moved into the server, the client's display helpers

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: The six forms' routing, and the sign-in controller

**Files:**
- Create: `examples/bookmarks/app/controllers/forms_routing.{hpp,cpp}`,
  `examples/bookmarks/app/controllers/session_controller.{hpp,cpp}`
- Create: `examples/bookmarks/tests/client_fixture.hpp`
- Test: `examples/bookmarks/tests/test_forms_routing.cpp`, `examples/bookmarks/tests/test_session_controller.cpp`

**Interfaces:**
- Consumes: `forms::{FormModel::forAction<A>, FormSession, Submitter, bridgeSubmitter, bridgeChoiceFetcher,
  SubmitMode}` (Part 5; `bridgeSubmitter` is right here: every bookmarks model keeps its state in the database, so
  its own handler per model sees what the screens' handlers see); `examples::mapCompletion`
  (`examples/common/app/completion_map.hpp`, Part 6); `async::Completion<T>::makeSettleable`;
  `async::CallbackScope::token()`; `bridge::Bridge::setDefaultSession`; `session::Context`;
  `bookmarks::{Login, LoginResult, AuthToken}`; `bookmarks::auth::setTokenIssuer`; `session::TokenIssuer(std::string,
  …)`, `session::hmacSha256`; `glz::read_json`, `glz::write_json`; `BackendRig`, `awaitQt` (`testkit/pump.hpp`).
- Produces: `bookmarks::client::{kFormActions, formModelFor, decodeLoginResult, formsSubmitter, loginSubmitter,
  Route, SessionController}`; `SessionController(reactive::Runtime&, bridge::Bridge&, exec::IExecutor&)` with
  `loginForm()`, `principal()`, `signedIn()`, `principalText()`, `route()`; `bookmarks::testing::{kSecret,
  authedRig, ScopedTokenIssuer, submitAndWait}`.

- [ ] **Step 1: Write the failing tests**

Create `examples/bookmarks/tests/client_fixture.hpp` (Tasks 11–14 extend it):

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/forms/engine/form_session.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

/// @file
/// The wiring bookmarks' client tests share: a rig whose bridge carries a signed session, and the waits.

namespace bookmarks::testing {

/// @brief The signing secret every test rig shares.
inline constexpr std::string_view kSecret = "bookmarks-client-test-secret";

/// @brief A rig with the rung's authorizer (Socket mode) and a signed session for @p principal installed.
/// @param mode The deployment shape.
/// @param principal Who the session belongs to.
/// @return The rig.
[[nodiscard]] inline std::unique_ptr<::morph::ladder::testkit::BackendRig> authedRig(
    ::morph::ladder::testkit::Mode mode, std::string principal = "alice") {
    auto const authorizer =
        std::make_shared<auth::BookmarksAuthorizer>(std::string{kSecret}, ::morph::session::hmacSha256);
    auto rig = std::make_unique<::morph::ladder::testkit::BackendRig>(mode, 1, authorizer);
    ::morph::session::TokenIssuer const issuer{std::string{kSecret}, ::morph::session::hmacSha256};
    ::morph::session::Context ctx;
    ctx.principal = std::move(principal);
    ctx.token = issuer.issue(
        ::morph::session::SessionToken{.principal = ctx.principal, .expiresAtMs = 4102444800000, .roles = {}});
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Installs the process-global issuer `AuthModel` mints Login tokens from, for one test.
class ScopedTokenIssuer {
public:
    ScopedTokenIssuer() {
        auth::setTokenIssuer(
            std::make_shared<::morph::session::TokenIssuer>(std::string{kSecret}, ::morph::session::hmacSha256));
    }
    ~ScopedTokenIssuer() { auth::setTokenIssuer(nullptr); }
    ScopedTokenIssuer(ScopedTokenIssuer const&) = delete;
    ScopedTokenIssuer& operator=(ScopedTokenIssuer const&) = delete;
    ScopedTokenIssuer(ScopedTokenIssuer&&) = delete;
    ScopedTokenIssuer& operator=(ScopedTokenIssuer&&) = delete;
};

/// @brief Submits @p form and waits for its reply, success or failure.
/// @param form A form whose body is complete.
inline void submitAndWait(::morph::forms::FormSession& form) {
    REQUIRE(form.ready());
    form.submit();
    REQUIRE(form.pending());
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return !form.pending(); }));
}

}  // namespace bookmarks::testing
```

Create `examples/bookmarks/tests/test_forms_routing.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <exception>
#include <glaze/glaze.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/control.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/models/tag_model.hpp"
#include "client_fixture.hpp"
#include "controllers/forms_routing.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief The failure message a submission ends with.
[[nodiscard]] std::string refusalOf(morph::async::Completion<std::string> reply) {
    std::exception_ptr failure;
    bool settled = false;
    reply.thenDetached([&](std::string const&) { settled = true; }).onErrorDetached([&](std::exception_ptr error) {
        failure = std::move(error);
        settled = true;
    });
    REQUIRE(pumpUntil([&] { return settled; }));
    REQUIRE(failure != nullptr);
    return morph::reactive::errorMessage(failure);
}

[[nodiscard]] std::int64_t tagIdNamed(bookmarks::ListTagsResult const& tags, std::string const& name) {
    for (auto const& tag : tags.tags) {
        if (tag.name == name) {
            return *tag.id;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("decodeLoginResult accepts a real Login reply and rejects anything that is not one",
          "[bookmarks][controller]") {
    auto const decoded = bookmarks::client::decodeLoginResult(R"({"token":"signed.token.value","principal":"alice"})");
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->token.hasValue());
    CHECK(*decoded->token == "signed.token.value");
    CHECK(decoded->principal == "alice");
    for (char const* body : {"", "not json at all", "[1,2,3]", "null", R"({"token":123,"principal":"alice"})",
                             R"({"principal":"alice")"}) {
        INFO("unexpectedly decoded: " << body);
        CHECK_FALSE(bookmarks::client::decodeLoginResult(body).has_value());
    }
}

TEST_CASE("Every one of the six forms reads its schema and submits explicitly", "[bookmarks][controller]") {
    for (std::string_view const actionType : bookmarks::client::kFormActions) {
        INFO(actionType);
        auto const model = bookmarks::client::formModelFor(actionType);
        CHECK(model.actionType() == actionType);
        CHECK(model.submitMode() == morph::forms::SubmitMode::Explicit);
    }
    // A read-only action carries no explicit submit: the six are explicit by declaration, not by default.
    auto const listing = morph::forms::FormModel::forAction<bookmarks::ListBookmarks>();
    CHECK(listing.submitMode() == morph::forms::SubmitMode::Automatic);
    CHECK_THROWS_AS(bookmarks::client::formModelFor("BulkEdit"), std::invalid_argument);
}

TEST_CASE("formsSubmitter refuses an action outside the six, naming it, and relays a model's refusal",
          "[bookmarks][controller]") {
    DbFixture fixture;
    auto rig = bookmarks::testing::authedRig(Mode::Local);
    auto submit = bookmarks::client::formsSubmitter(rig->bridge(0), *rig->executor());
    for (std::string_view const actionType : {std::string_view{"CreateBookmarks"}, std::string_view{"ListSharedFeed"}}) {
        std::string const message = refusalOf(submit(actionType, R"({"url":"https://typo.example"})"));
        CHECK(message.find("no model in this client serves action") != std::string::npos);
        CHECK(message.find(actionType) != std::string::npos);
    }
    std::string const refused = refusalOf(submit("CreateBookmark", R"({"url":""})"));
    CHECK(refused.find("CreateBookmark") != std::string::npos);
}

TEST_CASE("loginSubmitter routes each of the six form actions to the model that serves it",
          "[bookmarks][controller]") {
    DbFixture fixture;
    bookmarks::testing::ScopedTokenIssuer const issuer;
    BackendRig rig{Mode::Local, 1};
    morph::async::CallbackScope lifetime;
    std::string announced;
    auto submit = bookmarks::client::loginSubmitter(rig.bridge(0), *rig.executor(), lifetime,
                                                    [&](bookmarks::LoginResult const& result) {
                                                        announced = result.principal;
                                                    });

    std::string const login = awaitQt(submit("Login", R"({"username":"alice"})"));
    CHECK(announced == "alice");
    CHECK(login.find("\"principal\"") != std::string::npos);

    std::string const created = awaitQt(submit("CreateBookmark", R"({"url":"https://route.example","tags":["work","home"]})"));
    auto const createdResult = glz::read_json<bookmarks::CreateBookmarkResult>(created);
    REQUIRE(createdResult.has_value());
    std::int64_t const bookmarkId = *createdResult->id;

    static_cast<void>(awaitQt(submit(
        "EditBookmark", R"({"id":)" + std::to_string(bookmarkId) + R"(,"url":"https://edited.example","title":"Edited"})")));
    std::string const imported = awaitQt(submit(
        "ImportBookmarks",
        R"({"chunk":"<DT><A HREF=\"https://imported.example\">Imported</A>","opId":"import-op-1"})"));
    CHECK(imported.find("\"imported\"") != std::string::npos);

    auto tagHandler = rig.client<bookmarks::TagModel>(0);
    auto const before = awaitQt(tagHandler.execute(bookmarks::ListTags{}));
    std::int64_t const workId = tagIdNamed(before, "work");
    std::int64_t const homeId = tagIdNamed(before, "home");
    REQUIRE(workId > 0);
    REQUIRE(homeId > 0);
    static_cast<void>(awaitQt(submit("RenameTag", R"({"id":)" + std::to_string(workId) + R"(,"name":"office"})")));
    static_cast<void>(awaitQt(submit("MergeTags", R"({"sourceId":)" + std::to_string(homeId) +
                                                      R"(,"targetId":)" + std::to_string(workId) + "}")));
    auto const after = awaitQt(tagHandler.execute(bookmarks::ListTags{}));
    CHECK(after.tags.size() == 1);
    CHECK(tagIdNamed(after, "office") == workId);
}
```

Create `examples/bookmarks/tests/test_session_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>

#include "bookmarks/models/bookmark_model.hpp"
#include "client_fixture.hpp"
#include "controllers/forms_routing.hpp"
#include "controllers/session_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

}  // namespace

TEST_CASE("A Login reply is redacted, and the session is installed before it shows", "[bookmarks][controller]") {
    DbFixture fixture;
    bookmarks::testing::ScopedTokenIssuer const issuer;
    BackendRig rig{Mode::Local, 1};
    morph::reactive::Runtime runtime{*rig.executor()};
    bookmarks::client::SessionController session{runtime, rig.bridge(0), *rig.executor()};
    CHECK_FALSE(session.signedIn());
    CHECK(session.route() == bookmarks::client::Route::SignIn);
    CHECK(session.principalText() == "not signed in");

    // Records who was signed in at the moment the reply became visible to a view.
    std::optional<std::string> principalWhenShown;
    morph::reactive::Effect const watch{runtime, [&] {
                                            if (session.loginForm().lastReply().has_value()) {
                                                principalWhenShown = session.principal();
                                            }
                                        }};

    session.loginForm().prefill(R"({"username":"alice"})");
    bookmarks::testing::submitAndWait(session.loginForm());
    REQUIRE(pumpUntil([&] { return principalWhenShown.has_value(); }));
    CHECK(*principalWhenShown == "alice");
    CHECK(session.signedIn());
    CHECK(session.route() == bookmarks::client::Route::Library);
    CHECK(session.principalText() == "signed in as alice");

    auto const& reply = session.loginForm().lastReply();
    REQUIRE(reply.has_value());
    auto const decoded = bookmarks::client::decodeLoginResult(*reply);
    REQUIRE(decoded.has_value());
    CHECK(decoded->principal == "alice");
    CHECK_FALSE(decoded->token.hasValue());

    // The bridge now carries the session: an action that needs a principal succeeds through a handler
    // the login never touched.
    auto bookmarkHandler = rig.client<bookmarks::BookmarkModel>(0);
    auto const listed = morph::ladder::testkit::awaitQt(bookmarkHandler.execute(bookmarks::ListBookmarks{}));
    CHECK(listed.bookmarks.empty());
}

TEST_CASE("A refused login shows the model's message and stays signed out", "[bookmarks][controller]") {
    DbFixture fixture;
    bookmarks::testing::ScopedTokenIssuer const issuer;
    BackendRig rig{Mode::Local, 1};
    morph::reactive::Runtime runtime{*rig.executor()};
    bookmarks::client::SessionController session{runtime, rig.bridge(0), *rig.executor()};

    session.loginForm().prefill(R"({"username":"not a principal!"})");
    bookmarks::testing::submitAndWait(session.loginForm());
    CHECK(session.loginForm().lastError() != nullptr);
    CHECK_FALSE(session.signedIn());
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/all --target ladder_bookmarks_tests`
Expected: FAIL — `'controllers/forms_routing.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bookmarks/app/controllers/forms_routing.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/bridge_submitter.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "bookmarks/dto/auth_dto.hpp"

/// @file
/// How bookmarks' six forms reach their models: one submitter that routes Login, CreateBookmark,
/// EditBookmark, ImportBookmarks, RenameTag and MergeTags across AuthModel, BookmarkModel and TagModel, and
/// the Login wrapper that installs the session a successful login returns.

namespace bookmarks::client {

/// @brief The six action types a bookmarks form submits: the ones a user types. Every other action is
///        parameterised by a row the user picked, and goes through a controller's `Mutation`.
inline constexpr std::array<std::string_view, 6> kFormActions{"Login",           "CreateBookmark", "EditBookmark",
                                                              "ImportBookmarks", "RenameTag",      "MergeTags"};

/// @brief The runtime form model of one of `kFormActions`: `FormModel::forAction<A>()` of its action type.
/// @param actionType One of `kFormActions`.
/// @return The model.
/// @throws std::invalid_argument for any other action type.
[[nodiscard]] ::morph::forms::FormModel formModelFor(std::string_view actionType);

/// @brief Reads a `Login` reply.
/// @param resultJson The reply text.
/// @return The result, or `nullopt` when the text is not one.
[[nodiscard]] std::optional<LoginResult> decodeLoginResult(std::string const& resultJson);

/// @brief Submits `kFormActions` through the bridge's registry and refuses every other action type with
///        "no model in this client serves action '<type>'", so a misrouted form fails loudly.
/// @param bridge The client's bridge.
/// @param callbacks Where replies are delivered.
/// @return The submitter.
[[nodiscard]] ::morph::forms::Submitter formsSubmitter(::morph::bridge::Bridge& bridge,
                                                       ::morph::exec::IExecutor& callbacks);

/// @brief `formsSubmitter`, plus what a successful `Login` needs: its token installed as the bridge's
///        default session and @p onLogin told, both **before** the reply resolves — so by the time a view
///        sees the reply, every later call already carries the session. The reply a form keeps is
///        re-encoded without the token: a bearer credential has no reason to reach anything displayable.
/// @param bridge The client's bridge. Borrowed: it must outlive the submitter.
/// @param callbacks Where replies are delivered.
/// @param lifetime Gates the Login continuation; its owner's last member. Borrowed.
/// @param onLogin Told the result of each successful login.
/// @return The submitter.
[[nodiscard]] ::morph::forms::Submitter loginSubmitter(::morph::bridge::Bridge& bridge,
                                                       ::morph::exec::IExecutor& callbacks,
                                                       ::morph::async::CallbackScope const& lifetime,
                                                       std::function<void(LoginResult const&)> onLogin);

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/forms_routing.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/forms_routing.hpp"

#include <algorithm>
#include <exception>
#include <glaze/glaze.hpp>
#include <morph/core/completion.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <utility>

#include "app/completion_map.hpp"
#include "bookmarks/models/auth_model.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/tag_model.hpp"

namespace bookmarks::client {

namespace {

using Reply = ::morph::async::Completion<std::string>;
using ::morph::forms::FormModel;

[[nodiscard]] Reply refused(::morph::exec::IExecutor* deliver, std::string message) {
    auto [completion, promise] = Reply::makeSettleable(deliver);
    promise.reject(std::make_exception_ptr(std::runtime_error{std::move(message)}));
    return std::move(completion);
}

}  // namespace

::morph::forms::FormModel formModelFor(std::string_view actionType) {
    if (actionType == "Login") {
        return FormModel::forAction<Login>();
    }
    if (actionType == "CreateBookmark") {
        return FormModel::forAction<CreateBookmark>();
    }
    if (actionType == "EditBookmark") {
        return FormModel::forAction<EditBookmark>();
    }
    if (actionType == "ImportBookmarks") {
        return FormModel::forAction<ImportBookmarks>();
    }
    if (actionType == "RenameTag") {
        return FormModel::forAction<RenameTag>();
    }
    if (actionType == "MergeTags") {
        return FormModel::forAction<MergeTags>();
    }
    throw std::invalid_argument{"bookmarks: no form for action '" + std::string{actionType} + "'"};
}

std::optional<LoginResult> decodeLoginResult(std::string const& resultJson) {
    LoginResult result;
    if (glz::read_json(result, resultJson)) {
        return std::nullopt;
    }
    return result;
}

::morph::forms::Submitter formsSubmitter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor& callbacks) {
    return [routed = ::morph::forms::bridgeSubmitter(bridge, callbacks), deliver = &callbacks](
               std::string_view actionType, std::string body) -> Reply {
        if (std::ranges::find(kFormActions, actionType) == kFormActions.end()) {
            return refused(deliver, "no model in this client serves action '" + std::string{actionType} + "'");
        }
        return routed(actionType, std::move(body));
    };
}

::morph::forms::Submitter loginSubmitter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor& callbacks,
                                         ::morph::async::CallbackScope const& lifetime,
                                         std::function<void(LoginResult const&)> onLogin) {
    return [routed = formsSubmitter(bridge, callbacks), bridgePtr = &bridge, deliver = &callbacks,
            scope = &lifetime, onLogin = std::move(onLogin)](std::string_view actionType, std::string body) -> Reply {
        if (actionType != "Login") {
            return routed(actionType, std::move(body));
        }
        // The session is installed inside the mapping, before the derived reply resolves; a throw fails it.
        return ::morph::examples::mapCompletion<std::string>(
            *deliver, scope->token(), routed(actionType, std::move(body)),
            [bridgePtr, onLogin](std::string const& reply) {
                auto const result = decodeLoginResult(reply);
                if (!result.has_value()) {
                    throw std::runtime_error{"login succeeded but its reply could not be decoded"};
                }
                ::morph::session::Context session;
                session.principal = result->principal;
                session.token = result->token.hasValue() ? *result->token : std::string{};
                bridgePtr->setDefaultSession(session);
                onLogin(*result);
                LoginResult redacted = *result;
                redacted.token = AuthToken{};
                return glz::write_json(redacted).value_or(std::string{"{}"});
            },
            [](std::exception_ptr const&) {});
    };
}

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/session_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <string>

/// @file
/// Signing in: the Login form, and who the bridge's session now belongs to.

namespace bookmarks::client {

/// @brief Which screen the application shows.
enum class Route : std::uint8_t {
    SignIn,   ///< Nobody is signed in: the Login form.
    Library,  ///< Signed in: the collection, its tags and the shared feed.
};

/// @brief The sign-in screen's controller.
///
/// The Login form submits through `loginSubmitter`, which installs the returned token as the bridge's
/// default session before the reply resolves; the principal it announces is the server's, never what was
/// typed. Every library query is keyed on `signedIn()`, so none is issued before there is a session.
class SessionController {
public:
    /// @param runtime The runtime; its owner must be @p callbacks.
    /// @param bridge The client's bridge; the session is installed on it.
    /// @param callbacks Where replies are delivered.
    SessionController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                      ::morph::exec::IExecutor& callbacks);

    ~SessionController() = default;
    SessionController(SessionController const&) = delete;
    SessionController& operator=(SessionController const&) = delete;
    SessionController(SessionController&&) = delete;
    SessionController& operator=(SessionController&&) = delete;

    /// @brief The Login form.
    /// @return Its session.
    [[nodiscard]] ::morph::forms::FormSession& loginForm() noexcept { return _login; }

    /// @brief Who is signed in, as the server returned it. Tracked.
    /// @return The principal, or empty.
    [[nodiscard]] std::string const& principal() const { return _principal.get(); }

    /// @brief Whether a session is installed. Tracked.
    /// @return True after a successful login.
    [[nodiscard]] bool signedIn() const { return _signedIn.get(); }

    /// @brief "signed in as <principal>" or "not signed in". Tracked.
    /// @return The line.
    [[nodiscard]] std::string const& principalText() const { return _principalText.get(); }

    /// @brief The screen to show. Tracked.
    /// @return `Library` once signed in, else `SignIn`.
    [[nodiscard]] Route route() const { return signedIn() ? Route::Library : Route::SignIn; }

private:
    ::morph::reactive::Signal<std::string> _principal;
    ::morph::forms::FormSession _login;
    ::morph::reactive::Computed<bool> _signedIn;
    ::morph::reactive::Computed<std::string> _principalText;
    // Last: it gates the Login continuation, which writes `_principal`.
    ::morph::async::CallbackScope _lifetime;
};

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/session_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/session_controller.hpp"

#include <morph/forms/engine/bridge_submitter.hpp>

#include "controllers/forms_routing.hpp"

namespace bookmarks::client {

SessionController::SessionController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                                     ::morph::exec::IExecutor& callbacks)
    : _principal{runtime, std::string{}},
      _login{runtime, formModelFor("Login"),
             loginSubmitter(bridge, callbacks, _lifetime,
                            [this](LoginResult const& result) { _principal.set(result.principal); }),
             ::morph::forms::bridgeChoiceFetcher(bridge, callbacks)},
      _signedIn{runtime, [this] { return !_principal.get().empty(); }},
      _principalText{runtime, [this] {
                         return _signedIn.get() ? "signed in as " + _principal.get() : std::string{"not signed in"};
                     }} {}

}  // namespace bookmarks::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_bookmarks_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/bookmarks/ladder_bookmarks_tests "[bookmarks][controller]"
```

Expected: PASS. Mutation check: in `loginSubmitter`, delete `bridgePtr->setDefaultSession(session);`. Expected
FAIL in "A Login reply is redacted, and the session is installed before it shows" (the closing `ListBookmarks`
is refused: no principal). Restore. Second: delete `redacted.token = AuthToken{};`. Expected FAIL in the same case
(`CHECK_FALSE(decoded->token.hasValue())`). Restore. The ordering half — the principal is set before the reply is
visible — holds by construction (`mapCompletion` resolves the derived reply with the mapping's return value, after
the mapping returns); the `principalWhenShown` check pins it against a submitter that resolves synchronously.

- [ ] **Step 5: Commit**

```bash
git add examples/bookmarks/app/controllers examples/bookmarks/tests/client_fixture.hpp \
    examples/bookmarks/tests/test_forms_routing.cpp examples/bookmarks/tests/test_session_controller.cpp
git commit -m "wip(bookmarks): the six forms' routing and the sign-in controller

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: The tags and shared-feed controllers

**Files:**
- Create: `examples/bookmarks/app/controllers/shared_feed_controller.{hpp,cpp}`,
  `examples/bookmarks/app/controllers/tag_controller.{hpp,cpp}`
- Modify: `examples/bookmarks/tests/client_fixture.hpp` — `seedBookmark()`
- Test: `examples/bookmarks/tests/test_tag_and_feed_controllers.cpp`

**Interfaces:**
- Consumes: `Query`, `Computed`, `FormSession`, `examples::FormSuccess`; `formModelFor`, `formsSubmitter`
  (Task 10); `bookmark_text` (Task 9); `bookmarks::{SharedFeedModel, ListSharedFeed, TagModel, ListTags,
  TagSummary}`.
- Produces: `bookmarks::client::{FeedRow, SharedFeedController, TagRow, TagController}`;
  `SharedFeedController(Runtime&, Bridge&, IExecutor&, std::function<bool()> signedIn)` with `rows()`,
  `heading()`, `loading()`, `errorText()`, `hasError()`, `refresh()`; `TagController(Runtime&, Bridge&,
  IExecutor&, std::function<bool()> signedIn, std::function<void()> onChanged)` with the same plus
  `renameForm()`, `mergeForm()`; `bookmarks::testing::seedBookmark()`.

- [ ] **Step 1: Write the failing test**

Append to `examples/bookmarks/tests/client_fixture.hpp`, inside `namespace bookmarks::testing` (and add
`#include <vector>`, `#include "bookmarks/models/bookmark_model.hpp"`):

```cpp
/// @brief Creates a bookmark through the rig's own handler, outside any controller.
/// @param rig The rig whose session owns the bookmark.
/// @param url The url.
/// @param tags Tag names.
/// @param visibility Private or Shared.
/// @return Its id.
inline std::int64_t seedBookmark(::morph::ladder::testkit::BackendRig& rig, std::string url,
                                 std::vector<std::string> tags = {}, Visibility visibility = Visibility::Private) {
    auto handler = rig.client<BookmarkModel>(0);
    CreateBookmark create;
    create.url = std::move(url);
    create.tags = std::move(tags);
    create.visibility = visibility;
    BookmarkId const created = ::morph::ladder::testkit::awaitQt(handler.execute(create)).id;
    REQUIRE(created.hasValue());
    return *created;
}
```

Create `examples/bookmarks/tests/test_tag_and_feed_controllers.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/reactive/runtime.hpp>
#include <string>

#include "client_fixture.hpp"
#include "controllers/shared_feed_controller.hpp"
#include "controllers/tag_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using bookmarks::client::FeedRow;
using bookmarks::client::TagRow;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

[[nodiscard]] TagRow const* tagNamed(std::vector<TagRow> const& rows, std::string const& name) {
    auto const found = std::ranges::find_if(rows, [&](TagRow const& row) { return row.name == name; });
    return found == rows.end() ? nullptr : &*found;
}

/// @brief Whether a row type carries a bookmark's private notes.
template <class Row>
concept CarriesNotes = requires(Row const& row) { row.notes; };

}  // namespace

static_assert(!CarriesNotes<FeedRow>);

TEST_CASE("TagController lists every tag the caller owns, with its id and count, all three backend modes",
          "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = bookmarks::testing::authedRig(mode);
    static_cast<void>(bookmarks::testing::seedBookmark(*rig, "https://one.example", {"cpp", "rust"}));
    morph::reactive::Runtime runtime{*rig->executor()};
    bookmarks::client::TagController tags{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [] {}};

    REQUIRE(pumpUntil([&] { return tags.rows().size() == 2; }));
    TagRow const* cpp = tagNamed(tags.rows(), "cpp");
    REQUIRE(cpp != nullptr);
    CHECK(cpp->tagId > 0);
    CHECK(cpp->idText == "#" + std::to_string(cpp->tagId));
    CHECK(cpp->bookmarkCount == "1");
    CHECK(tags.heading() == "Tags (2)");
    CHECK_FALSE(tags.hasError());
}

TEST_CASE("TagController renames a tag through its form and tells the other panes, all three backend modes",
          "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = bookmarks::testing::authedRig(mode);
    static_cast<void>(bookmarks::testing::seedBookmark(*rig, "https://one.example", {"work"}));
    morph::reactive::Runtime runtime{*rig->executor()};
    int changes = 0;
    bookmarks::client::TagController tags{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [&] { ++changes; }};
    REQUIRE(pumpUntil([&] { return tags.rows().size() == 1; }));
    std::int64_t const workId = tags.rows().front().tagId;

    tags.renameForm().prefill(R"({"id":)" + std::to_string(workId) + R"(,"name":"office"})");
    bookmarks::testing::submitAndWait(tags.renameForm());
    REQUIRE(tags.renameForm().lastError() == nullptr);
    REQUIRE(pumpUntil([&] { return tagNamed(tags.rows(), "office") != nullptr; }));
    CHECK(tagNamed(tags.rows(), "office")->tagId == workId);
    CHECK(changes == 1);
    CHECK_FALSE(tags.renameForm().ready());  // reset after the success
}

TEST_CASE("TagController merges two tags through its form, all three backend modes", "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = bookmarks::testing::authedRig(mode);
    static_cast<void>(bookmarks::testing::seedBookmark(*rig, "https://one.example", {"cpp"}));
    static_cast<void>(bookmarks::testing::seedBookmark(*rig, "https://two.example", {"cxx"}));
    morph::reactive::Runtime runtime{*rig->executor()};
    bookmarks::client::TagController tags{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [] {}};
    REQUIRE(pumpUntil([&] { return tags.rows().size() == 2; }));
    std::int64_t const cppId = tagNamed(tags.rows(), "cpp")->tagId;
    std::int64_t const cxxId = tagNamed(tags.rows(), "cxx")->tagId;

    tags.mergeForm().prefill(R"({"sourceId":)" + std::to_string(cppId) + R"(,"targetId":)" + std::to_string(cxxId) + "}");
    bookmarks::testing::submitAndWait(tags.mergeForm());
    REQUIRE(pumpUntil([&] { return tags.rows().size() == 1; }));
    REQUIRE(tagNamed(tags.rows(), "cxx") != nullptr);
    CHECK(tagNamed(tags.rows(), "cxx")->bookmarkCount == "2");
    CHECK(tagNamed(tags.rows(), "cpp") == nullptr);
}

TEST_CASE("The tag forms show the model's refusal, and nothing changes", "[bookmarks][controller]") {
    DbFixture fixture;
    auto rig = bookmarks::testing::authedRig(Mode::Local);
    static_cast<void>(bookmarks::testing::seedBookmark(*rig, "https://one.example", {"cpp"}));
    morph::reactive::Runtime runtime{*rig->executor()};
    int changes = 0;
    bookmarks::client::TagController tags{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [&] { ++changes; }};
    REQUIRE(pumpUntil([&] { return tags.rows().size() == 1; }));
    std::string const cppId = std::to_string(tags.rows().front().tagId);

    // MergeTags::validate() refuses a tag merged into itself; the schema cannot say so.
    tags.mergeForm().prefill(R"({"sourceId":)" + cppId + R"(,"targetId":)" + cppId + "}");
    bookmarks::testing::submitAndWait(tags.mergeForm());
    CHECK(tags.mergeForm().lastError() != nullptr);
    CHECK(changes == 0);

    tags.renameForm().prefill(R"({"id":)" + cppId + R"(,"name":""})");
    CHECK_FALSE(tags.renameForm().ready());
}

TEST_CASE("SharedFeedController lists every shared bookmark and never a private one, all three backend modes",
          "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = bookmarks::testing::authedRig(mode);
    static_cast<void>(bookmarks::testing::seedBookmark(*rig, "https://alice-private.example"));
    std::int64_t const sharedId = bookmarks::testing::seedBookmark(*rig, "https://alice-shared.example", {},
                                                                  bookmarks::Visibility::Shared);
    morph::reactive::Runtime runtime{*rig->executor()};
    bookmarks::client::SharedFeedController feed{runtime, rig->bridge(0), *rig->executor(), [] { return true; }};

    REQUIRE(pumpUntil([&] { return feed.rows().size() == 1; }));
    CHECK(feed.rows().front().bookmarkId == sharedId);
    CHECK(feed.rows().front().line.starts_with("https://alice-shared.example · "));
    CHECK(feed.heading() == "Shared feed (1)");
}

TEST_CASE("Signed out, the tag and feed queries are idle; with no session at all they report, not crash",
          "[bookmarks][controller]") {
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    morph::reactive::Runtime runtime{*rig.executor()};
    {
        bookmarks::client::TagController tags{runtime, rig.bridge(0), *rig.executor(), [] { return false; }, [] {}};
        bookmarks::client::SharedFeedController feed{runtime, rig.bridge(0), *rig.executor(), [] { return false; }};
        CHECK_FALSE(tags.loading());
        CHECK_FALSE(feed.loading());
    }
    bookmarks::client::TagController tags{runtime, rig.bridge(0), *rig.executor(), [] { return true; }, [] {}};
    bookmarks::client::SharedFeedController feed{runtime, rig.bridge(0), *rig.executor(), [] { return true; }};
    REQUIRE(pumpUntil([&] { return tags.hasError() && feed.hasError(); }));
    CHECK_FALSE(tags.errorText().empty());
    CHECK_FALSE(feed.errorText().empty());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_bookmarks_tests`
Expected: FAIL — `'controllers/shared_feed_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bookmarks/app/controllers/shared_feed_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <string>
#include <vector>

#include "bookmarks/models/shared_feed_model.hpp"

/// @file
/// The cross-user shared feed.

namespace bookmarks::client {

/// @brief One shared bookmark, as the feed shows it. No notes: a feed row is a listing row.
struct FeedRow {
    std::int64_t bookmarkId = 0;  ///< The bookmark; also the row's key.
    std::string line;             ///< "title-or-url · created".

    /// @brief Field-wise equality.
    bool operator==(FeedRow const&) const = default;
};

/// @brief The shared feed's controller: one `Query<ListSharedFeed>`, idle while signed out.
class SharedFeedController {
public:
    /// @param runtime The runtime; its owner must be @p callbacks.
    /// @param bridge The client's bridge.
    /// @param callbacks Where replies are delivered.
    /// @param signedIn Tracked: whether a session is installed. The query is idle while it is false.
    SharedFeedController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                         ::morph::exec::IExecutor& callbacks, std::function<bool()> signedIn);

    ~SharedFeedController() = default;
    SharedFeedController(SharedFeedController const&) = delete;
    SharedFeedController& operator=(SharedFeedController const&) = delete;
    SharedFeedController(SharedFeedController&&) = delete;
    SharedFeedController& operator=(SharedFeedController&&) = delete;

    /// @brief The feed. Tracked.
    /// @return Its rows, newest first.
    [[nodiscard]] std::vector<FeedRow> const& rows() const { return _rows.get(); }

    /// @brief "Shared feed (N)". Tracked.
    /// @return The heading.
    [[nodiscard]] std::string const& heading() const { return _heading.get(); }

    /// @brief Whether a request is in flight. Tracked.
    /// @return True from issue to reply.
    [[nodiscard]] bool loading() const { return _feed.pending(); }

    /// @brief The last failure's text. Tracked.
    /// @return The text, or empty.
    [[nodiscard]] std::string const& errorText() const { return _errorText.get(); }

    /// @brief Whether the last request failed. Tracked.
    /// @return True after a failure, until the next success.
    [[nodiscard]] bool hasError() const { return !_errorText.get().empty(); }

    /// @brief Fetches the feed again.
    void refresh() { _feed.refetch(); }

private:
    ::morph::bridge::BridgeHandler<SharedFeedModel> _handler;
    std::function<bool()> _signedIn;
    ::morph::reactive::Query<ListSharedFeed> _feed;
    ::morph::reactive::Computed<std::vector<FeedRow>> _rows;
    ::morph::reactive::Computed<std::string> _heading;
    ::morph::reactive::Computed<std::string> _errorText;
};

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/shared_feed_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/shared_feed_controller.hpp"

#include <optional>
#include <utility>

#include "controllers/bookmark_text.hpp"

namespace bookmarks::client {

SharedFeedController::SharedFeedController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                                           ::morph::exec::IExecutor& callbacks, std::function<bool()> signedIn)
    : _handler{bridge, &callbacks},
      _signedIn{std::move(signedIn)},
      _feed{runtime, _handler,
            [this]() -> std::optional<ListSharedFeed> {
                return _signedIn() ? std::optional<ListSharedFeed>{ListSharedFeed{}} : std::nullopt;
            }},
      _rows{runtime,
            [this] {
                std::vector<FeedRow> rows;
                if (auto const& page = _feed.value()) {
                    for (auto const& summary : page->bookmarks) {
                        rows.push_back(FeedRow{.bookmarkId = summary.id.hasValue() ? *summary.id : 0,
                                               .line = headline(summary.title, summary.url) + " · " +
                                                       isoOrEmpty(summary.createdAt)});
                    }
                }
                return rows;
            }},
      _heading{runtime, [this] { return "Shared feed (" + std::to_string(_rows.get().size()) + ")"; }},
      _errorText{runtime, [this] {
                     auto const error = _feed.error();
                     return error != nullptr ? ::morph::reactive::errorMessage(error) : std::string{};
                 }} {}

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/tag_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <string>
#include <vector>

#include "app/form_success.hpp"
#include "bookmarks/models/tag_model.hpp"

/// @file
/// The caller's tags, and the rename and merge forms.

namespace bookmarks::client {

/// @brief One tag, as the tags pane shows it.
struct TagRow {
    std::int64_t tagId = 0;     ///< The tag; also the row's key.
    std::string idText;         ///< "#<id>": the number the rename and merge forms ask for.
    std::string name;           ///< The tag's name.
    std::string bookmarkCount;  ///< How many bookmarks carry it.

    /// @brief Field-wise equality.
    bool operator==(TagRow const&) const = default;
};

/// @brief The tags pane's controller: a `Query<ListTags>` and the RenameTag and MergeTags forms. A
///        successful rename or merge refetches the tags, resets its form and calls `onChanged`, because
///        the bookmarks, the open bookmark and the feed show tag names too.
class TagController {
public:
    /// @param runtime The runtime; its owner must be @p callbacks.
    /// @param bridge The client's bridge.
    /// @param callbacks Where replies are delivered.
    /// @param signedIn Tracked: whether a session is installed. The query is idle while it is false.
    /// @param onChanged Called after each successful rename or merge.
    TagController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                  ::morph::exec::IExecutor& callbacks, std::function<bool()> signedIn,
                  std::function<void()> onChanged);

    ~TagController() = default;
    TagController(TagController const&) = delete;
    TagController& operator=(TagController const&) = delete;
    TagController(TagController&&) = delete;
    TagController& operator=(TagController&&) = delete;

    /// @brief The caller's tags. Tracked.
    /// @return Their rows.
    [[nodiscard]] std::vector<TagRow> const& rows() const { return _rows.get(); }

    /// @brief "Tags (N)". Tracked.
    /// @return The heading.
    [[nodiscard]] std::string const& heading() const { return _heading.get(); }

    /// @brief Whether a request is in flight. Tracked.
    /// @return True from issue to reply.
    [[nodiscard]] bool loading() const { return _tags.pending(); }

    /// @brief The last listing failure's text. Tracked.
    /// @return The text, or empty.
    [[nodiscard]] std::string const& errorText() const { return _errorText.get(); }

    /// @brief Whether the last listing failed. Tracked.
    /// @return True after a failure, until the next success.
    [[nodiscard]] bool hasError() const { return !_errorText.get().empty(); }

    /// @brief Fetches the tags again.
    void refresh() { _tags.refetch(); }

    /// @brief The RenameTag form.
    /// @return Its session.
    [[nodiscard]] ::morph::forms::FormSession& renameForm() noexcept { return _rename; }

    /// @brief The MergeTags form.
    /// @return Its session.
    [[nodiscard]] ::morph::forms::FormSession& mergeForm() noexcept { return _merge; }

private:
    void onSubmitted(::morph::forms::FormSession& form);

    ::morph::bridge::BridgeHandler<TagModel> _handler;
    std::function<bool()> _signedIn;
    std::function<void()> _onChanged;
    ::morph::reactive::Query<ListTags> _tags;
    ::morph::forms::FormSession _rename;
    ::morph::forms::FormSession _merge;
    ::morph::reactive::Computed<std::vector<TagRow>> _rows;
    ::morph::reactive::Computed<std::string> _heading;
    ::morph::reactive::Computed<std::string> _errorText;
    ::morph::examples::FormSuccess _onRenamed;
    ::morph::examples::FormSuccess _onMerged;
};

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/tag_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/tag_controller.hpp"

#include <morph/forms/engine/bridge_submitter.hpp>
#include <optional>
#include <utility>

#include "controllers/bookmark_text.hpp"
#include "controllers/forms_routing.hpp"

namespace bookmarks::client {

TagController::TagController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                             ::morph::exec::IExecutor& callbacks, std::function<bool()> signedIn,
                             std::function<void()> onChanged)
    : _handler{bridge, &callbacks},
      _signedIn{std::move(signedIn)},
      _onChanged{std::move(onChanged)},
      _tags{runtime, _handler,
            [this]() -> std::optional<ListTags> {
                return _signedIn() ? std::optional<ListTags>{ListTags{}} : std::nullopt;
            }},
      _rename{runtime, formModelFor("RenameTag"), formsSubmitter(bridge, callbacks),
              ::morph::forms::bridgeChoiceFetcher(bridge, callbacks)},
      _merge{runtime, formModelFor("MergeTags"), formsSubmitter(bridge, callbacks),
             ::morph::forms::bridgeChoiceFetcher(bridge, callbacks)},
      _rows{runtime,
            [this] {
                std::vector<TagRow> rows;
                if (auto const& page = _tags.value()) {
                    for (auto const& tag : page->tags) {
                        std::int64_t const tagId = tag.id.hasValue() ? *tag.id : 0;
                        rows.push_back(TagRow{.tagId = tagId,
                                              .idText = "#" + std::to_string(tagId),
                                              .name = tag.name,
                                              .bookmarkCount = countText(tag.bookmarkCount)});
                    }
                }
                return rows;
            }},
      _heading{runtime, [this] { return "Tags (" + std::to_string(_rows.get().size()) + ")"; }},
      _errorText{runtime,
                 [this] {
                     auto const error = _tags.error();
                     return error != nullptr ? ::morph::reactive::errorMessage(error) : std::string{};
                 }},
      _onRenamed{runtime, _rename, [this] { onSubmitted(_rename); }},
      _onMerged{runtime, _merge, [this] { onSubmitted(_merge); }} {}

void TagController::onSubmitted(::morph::forms::FormSession& form) {
    _tags.refetch();
    form.reset();
    _onChanged();
}

}  // namespace bookmarks::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_bookmarks_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/bookmarks/ladder_bookmarks_tests "[bookmarks][controller]"
```

Expected: PASS. Mutation check: in `SharedFeedController`'s key, return `ListSharedFeed{}` unconditionally.
Expected FAIL in "Signed out, the tag and feed queries are idle…" (`CHECK_FALSE(feed.loading())`). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/bookmarks/app/controllers examples/bookmarks/tests/client_fixture.hpp \
    examples/bookmarks/tests/test_tag_and_feed_controllers.cpp
git commit -m "wip(bookmarks): the tags and shared-feed controllers

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 12: The bookmark list controller — create, archived toggle, multiple selection, bulk archive

**Files:**
- Create: `examples/bookmarks/app/controllers/bookmark_list_controller.{hpp,cpp}`
- Modify: `examples/bookmarks/tests/client_fixture.hpp` — `createVia()`
- Test: `examples/bookmarks/tests/test_bookmark_list_controller.cpp`

**Interfaces:**
- Consumes: `Query`, `Mutation`, `MutationOptions`, `Computed`, `Effect`, `FormSession`, `FormSuccess`;
  `formModelFor`, `formsSubmitter` (Task 10); `bookmark_text` (Task 9); `bookmarks::{BookmarkModel, ListBookmarks,
  ReadFilter, ArchiveFilter, BulkEdit, BulkArchiveOp, BulkEditResult, CreateBookmarkResult, ArchiveBookmark}`;
  `glz::read_json`.
- Produces: `bookmarks::client::{BookmarkRow, rowOf, BookmarkListController}`;
  `BookmarkListController(Runtime&, Bridge&, IExecutor&, std::function<bool()> signedIn,
  std::function<void()> onChanged)` with `rows()`, `countText()`, `loading()`, `refresh()`, `refreshAll()`,
  `includeArchived()`, `setIncludeArchived()`, `selection()`, `select()`, `selectionText()`, `canBulkEdit()`,
  `bulkArchive()`, `bulkUnarchive()`, `createForm()`, `lastCreatedId()`, `statusText()`, `statusIsError()`,
  `hasStatus()`; `bookmarks::testing::createVia()`.

- [ ] **Step 1: Write the failing test**

Append to `examples/bookmarks/tests/client_fixture.hpp`, inside `namespace bookmarks::testing` (and add
`#include "controllers/bookmark_list_controller.hpp"` and `#include <optional>`):

```cpp
/// @brief Prefills the create form with @p bodyJson, submits it and waits for the new id.
/// @param list The list controller whose form to use.
/// @param bodyJson A complete `CreateBookmark` body.
/// @return The new bookmark's id.
inline std::int64_t createVia(client::BookmarkListController& list, std::string const& bodyJson) {
    auto& form = list.createForm();
    form.prefill(bodyJson);
    std::optional<std::int64_t> const before = list.lastCreatedId();
    submitAndWait(form);
    REQUIRE(form.lastError() == nullptr);
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return list.lastCreatedId() != before; }));
    return list.lastCreatedId().value_or(0);
}
```

Create `examples/bookmarks/tests/test_bookmark_list_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <string>
#include <vector>

#include "bookmarks/models/bookmark_model.hpp"
#include "client_fixture.hpp"
#include "controllers/bookmark_list_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using bookmarks::client::BookmarkListController;
using bookmarks::client::BookmarkRow;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief A signed-in client and its list controller; `changes` counts `onChanged` calls.
struct ListFixture {
    explicit ListFixture(Mode mode)
        : rig{bookmarks::testing::authedRig(mode)},
          runtime{*rig->executor()},
          list{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [this] { ++changes; }} {}

    std::unique_ptr<BackendRig> rig;
    morph::reactive::Runtime runtime;
    int changes = 0;
    BookmarkListController list;
};

[[nodiscard]] BookmarkRow const* rowFor(BookmarkListController const& list, std::int64_t bookmarkId) {
    auto const found =
        std::ranges::find_if(list.rows(), [&](BookmarkRow const& row) { return row.bookmarkId == bookmarkId; });
    return found == list.rows().end() ? nullptr : &*found;
}

void reloadAndWait(BookmarkListController& list) {
    list.refresh();
    REQUIRE(pumpUntil([&] { return !list.loading(); }));
}

/// @brief Whether a row type carries a bookmark's private notes.
template <class Row>
concept CarriesNotes = requires(Row const& row) { row.notes; };

}  // namespace

static_assert(!CarriesNotes<BookmarkRow>);

TEST_CASE("The create form stores a bookmark, and the list shows it in the narrower row shape, all three backend modes",
          "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    ListFixture client{mode};
    std::int64_t const bookmarkId = bookmarks::testing::createVia(
        client.list, R"({"url":"https://row.example","title":"Row","notes":"must not leak"})");

    REQUIRE(pumpUntil([&] { return rowFor(client.list, bookmarkId) != nullptr; }));
    BookmarkRow const& row = *rowFor(client.list, bookmarkId);
    CHECK(row.title == "Row · https://row.example");
    CHECK(row.state == "Private · Active");
    CHECK(row.title.find("must not leak") == std::string::npos);
    CHECK(client.list.countText() == "1 bookmark(s)");
    CHECK(client.list.statusText() == "created");
    CHECK(client.changes == 1);
    CHECK_FALSE(client.list.createForm().ready());  // reset after the success
}

TEST_CASE("Archived bookmarks show only when asked for, all three backend modes", "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    ListFixture client{mode};
    std::int64_t const bookmarkId = bookmarks::testing::seedBookmark(*client.rig, "https://arms.example");
    auto handler = client.rig->client<bookmarks::BookmarkModel>(0);
    static_cast<void>(awaitQt(handler.execute(bookmarks::ArchiveBookmark{.id = bookmarks::BookmarkId{bookmarkId}})));

    reloadAndWait(client.list);
    CHECK(client.list.rows().empty());

    client.list.setIncludeArchived(true);
    REQUIRE(pumpUntil([&] { return rowFor(client.list, bookmarkId) != nullptr; }));
    CHECK(rowFor(client.list, bookmarkId)->state == "Private · Archived");
}

TEST_CASE("Bulk archive and unarchive act on the selection and clear it, all three backend modes",
          "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    ListFixture client{mode};
    std::int64_t const first = bookmarks::testing::seedBookmark(*client.rig, "https://bulk-one.example");
    std::int64_t const second = bookmarks::testing::seedBookmark(*client.rig, "https://bulk-two.example");
    reloadAndWait(client.list);
    REQUIRE(client.list.rows().size() == 2);

    client.list.select({first, second});
    CHECK(client.list.selectionText() == "2 selected");
    REQUIRE(client.list.canBulkEdit());
    client.list.bulkArchive();
    REQUIRE(pumpUntil([&] { return client.list.statusText() == "bulk edit affected 2 bookmark(s)"; }));
    CHECK(client.list.selection().empty());
    REQUIRE(pumpUntil([&] { return client.list.rows().empty(); }));

    client.list.setIncludeArchived(true);
    REQUIRE(pumpUntil([&] { return client.list.rows().size() == 2; }));
    for (auto const& row : client.list.rows()) {
        CHECK(row.state == "Private · Archived");
    }

    client.list.select({first, second});
    client.list.bulkUnarchive();
    REQUIRE(pumpUntil([&] {
        return client.list.selection().empty() && client.list.rows().size() == 2 &&
               std::ranges::all_of(client.list.rows(), [](BookmarkRow const& row) { return row.state == "Private · Active"; });
    }));
}

TEST_CASE("With nothing selected the bulk actions issue nothing", "[bookmarks][controller]") {
    DbFixture fixture;
    ListFixture client{Mode::Local};
    REQUIRE(pumpUntil([&] { return !client.list.loading(); }));
    CHECK_FALSE(client.list.canBulkEdit());
    client.list.bulkArchive();
    client.list.bulkUnarchive();
    CHECK_FALSE(client.list.hasStatus());
    CHECK_FALSE(client.list.loading());
}

TEST_CASE("The create form refuses an empty url, and shows a refusal its schema cannot see",
          "[bookmarks][controller]") {
    DbFixture fixture;
    ListFixture client{Mode::Local};
    auto& form = client.list.createForm();
    form.prefill(R"({"url":""})");
    CHECK_FALSE(form.ready());

    form.prefill(R"({"url":"https://)" + std::string(bookmarks::kMaxUrlBytes, 'x') + R"("})");
    bookmarks::testing::submitAndWait(form);
    REQUIRE(form.lastError() != nullptr);
    CHECK(morph::reactive::errorMessage(form.lastError()).find("CreateBookmark") != std::string::npos);
    CHECK(client.changes == 0);
}

TEST_CASE("Signed out the list is idle; with no session at all it reports, not crashes", "[bookmarks][controller]") {
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    morph::reactive::Runtime runtime{*rig.executor()};
    {
        BookmarkListController idle{runtime, rig.bridge(0), *rig.executor(), [] { return false; }, [] {}};
        CHECK_FALSE(idle.loading());
    }
    BookmarkListController list{runtime, rig.bridge(0), *rig.executor(), [] { return true; }, [] {}};
    REQUIRE(pumpUntil([&] { return list.statusIsError(); }));
    CHECK_FALSE(list.statusText().empty());
}

TEST_CASE("a bookmark list destroyed with a create in flight drops its reply", "[bookmarks][controller]") {
    DbFixture fixture;
    auto rig = bookmarks::testing::authedRig(Mode::LocalSingleThread);
    morph::reactive::Runtime runtime{*rig->executor()};
    {
        BookmarkListController abandoned{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [] {}};
        abandoned.createForm().prefill(R"({"url":"https://abandoned.example"})");
        abandoned.createForm().submit();
        // No pump before this brace: the reply cannot have been delivered yet.
    }
    BookmarkListController live{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [] {}};
    static_cast<void>(bookmarks::testing::createVia(live, R"({"url":"https://live.example"})"));
    REQUIRE(pumpUntil([&] { return live.rows().size() == 2; }));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_bookmarks_tests`
Expected: FAIL — `'controllers/bookmark_list_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bookmarks/app/controllers/bookmark_list_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/form_success.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "controllers/bookmark_text.hpp"

/// @file
/// The signed-in user's collection: the create form, the listing with its archived toggle, and bulk
/// archive/unarchive over a multiple selection.

namespace bookmarks::client {

/// @brief One bookmark in the listing. No notes and no description: a listing never shows them.
struct BookmarkRow {
    std::int64_t bookmarkId = 0;  ///< The bookmark; also the row's key.
    std::string title;            ///< `rowTitle(title, url)`.
    std::string state;            ///< "Private · Active" and the like.

    /// @brief Field-wise equality.
    bool operator==(BookmarkRow const&) const = default;
};

/// @brief One listing row.
/// @param summary The row as `ListBookmarks` returned it.
/// @return The display row.
[[nodiscard]] BookmarkRow rowOf(BookmarkSummary const& summary);

/// @brief The list pane's controller.
///
/// The listing is a `Query<ListBookmarks>` keyed on `signedIn` and the archived toggle, so flipping the
/// toggle refetches by itself and nothing is issued before sign-in. Bulk archive/unarchive is one
/// `Mutation<BulkEdit>` over the selection; a success clears the selection and refetches the listing. A
/// successful create resets the form, refetches the listing and calls `onChanged` — the tags and the feed
/// show the new bookmark too.
class BookmarkListController {
public:
    /// @param runtime The runtime; its owner must be @p callbacks.
    /// @param bridge The client's bridge.
    /// @param callbacks Where replies are delivered.
    /// @param signedIn Tracked: whether a session is installed.
    /// @param onChanged Called after a create or a bulk edit, for the other panes.
    BookmarkListController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                           ::morph::exec::IExecutor& callbacks, std::function<bool()> signedIn,
                           std::function<void()> onChanged);

    ~BookmarkListController() = default;
    BookmarkListController(BookmarkListController const&) = delete;
    BookmarkListController& operator=(BookmarkListController const&) = delete;
    BookmarkListController(BookmarkListController&&) = delete;
    BookmarkListController& operator=(BookmarkListController&&) = delete;

    /// @brief The listing. Tracked.
    /// @return Its rows, newest first.
    [[nodiscard]] std::vector<BookmarkRow> const& rows() const { return _rows.get(); }

    /// @brief "N bookmark(s)". Tracked.
    /// @return The count line.
    [[nodiscard]] std::string const& countText() const { return _countText.get(); }

    /// @brief Whether a listing request is in flight. Tracked.
    /// @return True from issue to reply.
    [[nodiscard]] bool loading() const { return _list.pending(); }

    /// @brief Fetches the listing again.
    void refresh() { _list.refetch(); }

    /// @brief Fetches the listing and asks the other panes to refresh too.
    void refreshAll() {
        _list.refetch();
        _onChanged();
    }

    /// @brief Whether archived bookmarks are listed. Tracked.
    /// @return The toggle.
    [[nodiscard]] bool includeArchived() const { return _includeArchived.get(); }

    /// @brief Sets the archived toggle; the listing refetches when it changes.
    /// @param include Whether to list archived bookmarks.
    void setIncludeArchived(bool include) { _includeArchived.set(include); }

    /// @brief The selected bookmarks. Tracked.
    /// @return Their ids.
    [[nodiscard]] std::vector<std::int64_t> const& selection() const { return _selection.get(); }

    /// @brief Replaces the selection.
    /// @param bookmarkIds The selected bookmarks.
    void select(std::vector<std::int64_t> bookmarkIds) { _selection.set(std::move(bookmarkIds)); }

    /// @brief "N selected". Tracked.
    /// @return The line.
    [[nodiscard]] std::string const& selectionText() const { return _selectionText.get(); }

    /// @brief Whether the bulk buttons act. Tracked.
    /// @return True while something is selected and no bulk edit is in flight.
    [[nodiscard]] bool canBulkEdit() const { return _canBulkEdit.get(); }

    /// @brief Archives every selected bookmark; does nothing with an empty selection.
    void bulkArchive() { bulk(BulkArchiveOp::Archive); }

    /// @brief Unarchives every selected bookmark; does nothing with an empty selection.
    void bulkUnarchive() { bulk(BulkArchiveOp::Unarchive); }

    /// @brief The CreateBookmark form.
    /// @return Its session.
    [[nodiscard]] ::morph::forms::FormSession& createForm() noexcept { return _create; }

    /// @brief The id the last successful create returned. Tracked.
    /// @return It, or `nullopt` before the first.
    [[nodiscard]] std::optional<std::int64_t> const& lastCreatedId() const { return _lastCreated.get(); }

    /// @brief The status line's text. Tracked.
    /// @return The text, or empty.
    [[nodiscard]] std::string const& statusText() const { return _status.get().text; }

    /// @brief Whether the status line reports a failure. Tracked.
    /// @return True for a failure.
    [[nodiscard]] bool statusIsError() const { return _status.get().isError; }

    /// @brief Whether there is a status to show. Tracked.
    /// @return True when the text is non-empty.
    [[nodiscard]] bool hasStatus() const { return !_status.get().text.empty(); }

private:
    void bulk(BulkArchiveOp operation);
    void onCreated();

    ::morph::reactive::Runtime* _rt;
    ::morph::bridge::BridgeHandler<BookmarkModel> _handler;
    std::function<bool()> _signedIn;
    std::function<void()> _onChanged;
    ::morph::reactive::Signal<bool> _includeArchived;
    ::morph::reactive::Signal<std::vector<std::int64_t>> _selection;
    ::morph::reactive::Signal<StatusLine> _status;
    ::morph::reactive::Signal<std::optional<std::int64_t>> _lastCreated;
    ::morph::reactive::Query<ListBookmarks> _list;
    ::morph::reactive::Mutation<BulkEdit> _bulk;
    ::morph::forms::FormSession _create;
    ::morph::reactive::Computed<std::vector<BookmarkRow>> _rows;
    ::morph::reactive::Computed<std::string> _countText;
    ::morph::reactive::Computed<std::string> _selectionText;
    ::morph::reactive::Computed<bool> _canBulkEdit;
    ::morph::reactive::Effect _onListFailed;
    ::morph::reactive::Effect _onBulkDone;
    ::morph::reactive::Effect _onBulkFailed;
    // Last: it calls into everything above.
    ::morph::examples::FormSuccess _onCreated;
};

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/bookmark_list_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/bookmark_list_controller.hpp"

#include <glaze/glaze.hpp>
#include <morph/forms/engine/bridge_submitter.hpp>

#include "controllers/forms_routing.hpp"

namespace bookmarks::client {

BookmarkRow rowOf(BookmarkSummary const& summary) {
    return BookmarkRow{.bookmarkId = summary.id.hasValue() ? *summary.id : 0,
                       .title = rowTitle(summary.title, summary.url),
                       .state = visibilityText(summary.visibility) + " · " + archiveStateText(summary.archiveState)};
}

BookmarkListController::BookmarkListController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                                               ::morph::exec::IExecutor& callbacks, std::function<bool()> signedIn,
                                               std::function<void()> onChanged)
    : _rt{&runtime},
      _handler{bridge, &callbacks},
      _signedIn{std::move(signedIn)},
      _onChanged{std::move(onChanged)},
      _includeArchived{runtime, false},
      _selection{runtime, std::vector<std::int64_t>{}},
      _status{runtime, StatusLine{}},
      _lastCreated{runtime, std::nullopt},
      _list{runtime, _handler,
            [this]() -> std::optional<ListBookmarks> {
                if (!_signedIn()) {
                    return std::nullopt;
                }
                return ListBookmarks{.cursor = {},
                                     .readFilter = ReadFilter::Any,
                                     .archiveFilter =
                                         _includeArchived.get() ? ArchiveFilter::Any : ArchiveFilter::ActiveOnly,
                                     .tag = {},
                                     .searchText = {}};
            }},
      _bulk{runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_list}}},
      _create{runtime, formModelFor("CreateBookmark"), formsSubmitter(bridge, callbacks),
              ::morph::forms::bridgeChoiceFetcher(bridge, callbacks)},
      _rows{runtime,
            [this] {
                std::vector<BookmarkRow> rows;
                if (auto const& page = _list.value()) {
                    for (auto const& summary : page->bookmarks) {
                        rows.push_back(rowOf(summary));
                    }
                }
                return rows;
            }},
      _countText{runtime, [this] { return std::to_string(_rows.get().size()) + " bookmark(s)"; }},
      _selectionText{runtime, [this] { return std::to_string(_selection.get().size()) + " selected"; }},
      _canBulkEdit{runtime, [this] { return !_selection.get().empty() && !_bulk.pending(); }},
      _onListFailed{runtime,
                    [this] {
                        if (auto const error = _list.error()) {
                            _status.set(failureLine(error));
                        }
                    }},
      _onBulkDone{runtime,
                  [this] {
                      auto const& done = _bulk.lastResult();
                      if (!done.has_value()) {
                          return;
                      }
                      _rt->untracked([&] {
                          _selection.set({});
                          _status.set(infoLine("bulk edit affected " + countText(done->affected) + " bookmark(s)"));
                          _onChanged();
                      });
                  }},
      _onBulkFailed{runtime,
                    [this] {
                        if (auto const error = _bulk.error()) {
                            _status.set(failureLine(error));
                        }
                    }},
      _onCreated{runtime, _create, [this] { onCreated(); }} {}

void BookmarkListController::bulk(BulkArchiveOp operation) {
    auto const& selected = _selection.peek();
    if (selected.empty()) {
        return;
    }
    BulkEdit edit{.ids = {}, .addTags = {}, .removeTags = {}, .archive = operation};
    for (std::int64_t const bookmarkId : selected) {
        edit.ids.emplace_back(bookmarkId);
    }
    _bulk.run(std::move(edit));
}

void BookmarkListController::onCreated() {
    if (auto const& reply = _create.lastReply(); reply.has_value()) {
        CreateBookmarkResult result;
        if (!glz::read_json(result, *reply) && result.id.hasValue()) {
            _lastCreated.set(*result.id);
        }
    }
    _status.set(infoLine("created"));
    _create.reset();
    _list.refetch();
    _onChanged();
}

}  // namespace bookmarks::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_bookmarks_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/bookmarks/ladder_bookmarks_tests "[bookmarks][controller]"
```

Expected: PASS. Mutation check: in `_onBulkDone`, delete `_selection.set({});`. Expected FAIL in "Bulk archive
and unarchive act on the selection and clear it" (`CHECK(client.list.selection().empty())`). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/bookmarks/app/controllers examples/bookmarks/tests/client_fixture.hpp \
    examples/bookmarks/tests/test_bookmark_list_controller.cpp
git commit -m "wip(bookmarks): the list controller — create, archived toggle, bulk archive

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: The bookmark detail controller — open, archive, delete, edit, import

**Files:**
- Create: `examples/bookmarks/app/controllers/bookmark_detail_controller.{hpp,cpp}`
- Modify: `examples/bookmarks/tests/client_fixture.hpp` — `openAndWait()`
- Test: `examples/bookmarks/tests/test_bookmark_detail_controller.cpp`

**Interfaces:**
- Consumes: as Task 12, plus `bookmarks::{GetBookmark, BookmarkView, ArchiveBookmark, UnarchiveBookmark,
  DeleteBookmark, EditBookmark, ImportBookmarks, ImportBookmarksResult, ImportOpId}`;
  `examples::newUuid()` (`examples/common/app/uuid.hpp`); `glz::write_json`.
- Produces: `bookmarks::client::{BookmarkFact, factsOf, editBodyOf, importBodyWith, BookmarkDetailController}`;
  `BookmarkDetailController(Runtime&, Bridge&, IExecutor&, std::function<bool()> signedIn,
  std::function<void()> onChanged)` with `open()`, `close()`, `refresh()`, `current()`, `hasCurrent()`,
  `detailTitle()`, `facts()`, `canAct()`, `archive()`, `unarchive()`, `remove()`, `editForm()`, `importForm()`,
  `importOpId()`, `loading()`, `statusText()`, `statusIsError()`, `hasStatus()`; `bookmarks::testing::openAndWait()`.

- [ ] **Step 1: Write the failing test**

Append to `examples/bookmarks/tests/client_fixture.hpp`, inside `namespace bookmarks::testing` (and add
`#include "controllers/bookmark_detail_controller.hpp"`):

```cpp
/// @brief Opens @p bookmarkId and waits until it is the open bookmark.
/// @param detail The detail controller.
/// @param bookmarkId The bookmark.
inline void openAndWait(client::BookmarkDetailController& detail, std::int64_t bookmarkId) {
    detail.open(bookmarkId);
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] {
        auto const& current = detail.current();
        return current.has_value() && current->id == BookmarkId{bookmarkId};
    }));
}
```

Create `examples/bookmarks/tests/test_bookmark_detail_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <string>
#include <vector>

#include "bookmarks/models/bookmark_model.hpp"
#include "client_fixture.hpp"
#include "controllers/bookmark_detail_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using bookmarks::client::BookmarkDetailController;
using bookmarks::client::BookmarkFact;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief A signed-in client and its detail controller; `changes` counts `onChanged` calls.
struct DetailFixture {
    explicit DetailFixture(Mode mode)
        : rig{bookmarks::testing::authedRig(mode)},
          runtime{*rig->executor()},
          detail{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [this] { ++changes; }} {}

    std::unique_ptr<BackendRig> rig;
    morph::reactive::Runtime runtime;
    int changes = 0;
    BookmarkDetailController detail;
};

[[nodiscard]] std::int64_t createFull(BackendRig& rig, bookmarks::CreateBookmark create) {
    auto handler = rig.client<bookmarks::BookmarkModel>(0);
    return *awaitQt(handler.execute(create)).id;
}

[[nodiscard]] std::string lineFor(BookmarkDetailController const& detail, std::string const& key) {
    for (BookmarkFact const& fact : detail.facts()) {
        if (fact.key == key) {
            return fact.line;
        }
    }
    return {};
}

}  // namespace

TEST_CASE("Opening a bookmark shows every fact, all three backend modes", "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    DetailFixture client{mode};
    CHECK_FALSE(client.detail.hasCurrent());
    CHECK_FALSE(client.detail.canAct());
    std::int64_t const bookmarkId = createFull(*client.rig, bookmarks::CreateBookmark{.url = "https://bag.example",
                                                                                      .title = "Bag",
                                                                                      .description = "desc",
                                                                                      .notes = "private note",
                                                                                      .tags = {"work", "home"}});
    bookmarks::testing::openAndWait(client.detail, bookmarkId);

    std::vector<std::string> keys;
    for (BookmarkFact const& fact : client.detail.facts()) {
        keys.push_back(fact.key);
    }
    CHECK(keys == std::vector<std::string>{"url", "description", "notes", "tags", "visibility", "read", "archive",
                                           "created", "updated"});
    CHECK(client.detail.detailTitle() == "Bag");
    CHECK(lineFor(client.detail, "url") == "url: https://bag.example");
    CHECK(lineFor(client.detail, "notes") == "notes: private note");
    CHECK(lineFor(client.detail, "tags").find("work") != std::string::npos);
    CHECK(lineFor(client.detail, "visibility") == "visibility: Private");
    CHECK(lineFor(client.detail, "read") == "read: Unread");
    CHECK(lineFor(client.detail, "archive") == "archive: Active");
    CHECK(lineFor(client.detail, "created").back() == 'Z');
    CHECK(client.detail.canAct());
}

TEST_CASE("The edit form is prefilled from the open bookmark, and a save shows on reload, all three backend modes",
          "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    DetailFixture client{mode};
    std::int64_t const bookmarkId =
        createFull(*client.rig, bookmarks::CreateBookmark{.url = "https://before.example", .title = "Before"});
    bookmarks::testing::openAndWait(client.detail, bookmarkId);

    auto& edit = client.detail.editForm();
    REQUIRE(pumpUntil([&] { return edit.body().has_value(); }));
    CHECK(edit.body()->find(R"("id":)" + std::to_string(bookmarkId)) != std::string::npos);
    CHECK(edit.body()->find("https://before.example") != std::string::npos);

    edit.prefill(R"({"id":)" + std::to_string(bookmarkId) + R"(,"url":"https://after.example","title":"After"})");
    bookmarks::testing::submitAndWait(edit);
    REQUIRE(edit.lastError() == nullptr);
    REQUIRE(pumpUntil([&] { return client.detail.detailTitle() == "After"; }));
    CHECK(lineFor(client.detail, "url") == "url: https://after.example");
    CHECK(client.detail.statusText() == "saved");
    CHECK(client.changes >= 1);
}

TEST_CASE("A background refetch never overwrites what the user is typing into the edit form",
          "[bookmarks][controller]") {
    DbFixture fixture;
    DetailFixture client{Mode::Local};
    std::int64_t const bookmarkId = bookmarks::testing::seedBookmark(*client.rig, "https://typed.example");
    bookmarks::testing::openAndWait(client.detail, bookmarkId);
    auto& edit = client.detail.editForm();
    REQUIRE(pumpUntil([&] { return edit.body().has_value(); }));

    // Stands in for the user typing a title.
    edit.prefill(R"({"id":)" + std::to_string(bookmarkId) + R"(,"url":"https://typed.example","title":"Typing"})");
    client.detail.refresh();
    REQUIRE(client.detail.loading());
    REQUIRE(pumpUntil([&] { return !client.detail.loading(); }));
    REQUIRE(edit.body().has_value());
    CHECK(edit.body()->find("Typing") != std::string::npos);
}

TEST_CASE("Archive then unarchive the open bookmark, all three backend modes", "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    DetailFixture client{mode};
    std::int64_t const bookmarkId = bookmarks::testing::seedBookmark(*client.rig, "https://arms.example", {},
                                                                    bookmarks::Visibility::Shared);
    bookmarks::testing::openAndWait(client.detail, bookmarkId);
    CHECK(lineFor(client.detail, "visibility") == "visibility: Shared");

    client.detail.archive();
    REQUIRE(pumpUntil([&] { return lineFor(client.detail, "archive") == "archive: Archived"; }));
    CHECK(client.detail.statusText() == "archived");
    client.detail.unarchive();
    REQUIRE(pumpUntil([&] { return lineFor(client.detail, "archive") == "archive: Active"; }));
    CHECK(client.detail.statusText() == "unarchived");
    CHECK(client.changes >= 2);
}

TEST_CASE("Delete closes the detail, and opening it again fails, all three backend modes", "[bookmarks][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    DetailFixture client{mode};
    std::int64_t const bookmarkId = bookmarks::testing::seedBookmark(*client.rig, "https://doomed.example");
    bookmarks::testing::openAndWait(client.detail, bookmarkId);

    client.detail.remove();
    REQUIRE(pumpUntil([&] { return !client.detail.hasCurrent() && client.detail.statusText() == "deleted"; }));
    client.detail.open(bookmarkId);
    REQUIRE(pumpUntil([&] { return client.detail.statusIsError(); }));
    CHECK_FALSE(client.detail.statusText().empty());
}

TEST_CASE("An unknown id reports the model's message, and with nothing open the actions issue nothing",
          "[bookmarks][controller]") {
    DbFixture fixture;
    DetailFixture client{Mode::Local};
    client.detail.archive();
    client.detail.unarchive();
    client.detail.remove();
    CHECK_FALSE(client.detail.hasStatus());

    client.detail.open(999999);
    REQUIRE(pumpUntil([&] { return client.detail.statusIsError(); }));
    CHECK_FALSE(client.detail.hasCurrent());
}

TEST_CASE("The import form starts with a fresh opId, and an import shows its counts and draws a new one",
          "[bookmarks][controller]") {
    DbFixture fixture;
    DetailFixture client{Mode::Local};
    std::string const firstOpId = client.detail.importOpId();
    CHECK(firstOpId.size() == 36);  // RFC 4122 text form

    client.detail.importForm().prefill(
        R"({"chunk":"<DT><A HREF=\"https://imported.example\">Imported</A>","opId":")" + firstOpId + R"("})");
    bookmarks::testing::submitAndWait(client.detail.importForm());
    REQUIRE(client.detail.importForm().lastError() == nullptr);
    REQUIRE(pumpUntil([&] { return client.detail.statusText() == "imported 1, skipped 0"; }));
    CHECK(client.detail.importOpId() != firstOpId);
    CHECK(client.changes == 1);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_bookmarks_tests`
Expected: FAIL — `'controllers/bookmark_detail_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bookmarks/app/controllers/bookmark_detail_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <vector>

#include "app/form_success.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "controllers/bookmark_text.hpp"

/// @file
/// The open bookmark: its facts, archive/unarchive/delete, the edit form and the import form.

namespace bookmarks::client {

/// @brief One labelled fact about the open bookmark, with the line the view shows.
struct BookmarkFact {
    std::string key;   ///< The fact's name; also its row key.
    std::string line;  ///< "key: value".

    /// @brief Field-wise equality.
    bool operator==(BookmarkFact const&) const = default;
};

/// @brief The open bookmark's facts in display order: url, description, notes, tags, visibility, read,
///        archive, created, updated.
/// @param view The bookmark.
/// @return The facts.
[[nodiscard]] std::vector<BookmarkFact> factsOf(BookmarkView const& view);

/// @brief The `EditBookmark` body that leaves @p view as it is — what the edit form starts from.
/// @param view The bookmark.
/// @return The JSON body.
[[nodiscard]] std::string editBodyOf(BookmarkView const& view);

/// @brief An `ImportBookmarks` body with no chunk yet and @p opId as its idempotency key.
/// @param opId The key.
/// @return The JSON body.
[[nodiscard]] std::string importBodyWith(std::string const& opId);

/// @brief The detail pane's controller.
///
/// The open bookmark is a `Query<GetBookmark>` keyed on the open id (and on `signedIn`). Archive and
/// unarchive invalidate it; delete closes it. The edit form is prefilled from each newly loaded bookmark —
/// once per bookmark, so a background refetch never overwrites what the user is typing — and the import
/// form carries a fresh UUID as its `opId`, drawn again after every successful import.
class BookmarkDetailController {
public:
    /// @param runtime The runtime; its owner must be @p callbacks.
    /// @param bridge The client's bridge.
    /// @param callbacks Where replies are delivered.
    /// @param signedIn Tracked: whether a session is installed.
    /// @param onChanged Called after an archive, unarchive, delete, save or import, for the other panes.
    BookmarkDetailController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                             ::morph::exec::IExecutor& callbacks, std::function<bool()> signedIn,
                             std::function<void()> onChanged);

    ~BookmarkDetailController() = default;
    BookmarkDetailController(BookmarkDetailController const&) = delete;
    BookmarkDetailController& operator=(BookmarkDetailController const&) = delete;
    BookmarkDetailController(BookmarkDetailController&&) = delete;
    BookmarkDetailController& operator=(BookmarkDetailController&&) = delete;

    /// @brief Opens a bookmark; opening the open one again refetches it.
    /// @param bookmarkId The bookmark.
    void open(std::int64_t bookmarkId);

    /// @brief Closes the open bookmark.
    void close();

    /// @brief Fetches the open bookmark again; does nothing while none is open.
    void refresh() { _detail.refetch(); }

    /// @brief Whether the open bookmark is being fetched. Tracked.
    /// @return True from issue to reply.
    [[nodiscard]] bool loading() const { return _detail.pending(); }

    /// @brief The open bookmark. Tracked.
    /// @return It, or `nullopt`.
    [[nodiscard]] std::optional<BookmarkView> const& current() const { return _detail.value(); }

    /// @brief Whether a bookmark is open and loaded. Tracked.
    /// @return True while `current()` is engaged.
    [[nodiscard]] bool hasCurrent() const { return _detail.value().has_value(); }

    /// @brief The pane's title: the open bookmark's headline, or a hint. Tracked.
    /// @return The title.
    [[nodiscard]] std::string const& detailTitle() const { return _title.get(); }

    /// @brief The open bookmark's facts. Tracked.
    /// @return `factsOf(*current())`, or empty.
    [[nodiscard]] std::vector<BookmarkFact> const& facts() const { return _facts.get(); }

    /// @brief Whether the archive/unarchive/delete buttons act. Tracked.
    /// @return True while a bookmark is loaded and none of the three is in flight.
    [[nodiscard]] bool canAct() const { return _canAct.get(); }

    /// @brief Archives the open bookmark; does nothing while none is loaded.
    void archive();

    /// @brief Unarchives the open bookmark; does nothing while none is loaded.
    void unarchive();

    /// @brief Deletes the open bookmark; does nothing while none is loaded.
    void remove();

    /// @brief The EditBookmark form.
    /// @return Its session.
    [[nodiscard]] ::morph::forms::FormSession& editForm() noexcept { return _edit; }

    /// @brief The ImportBookmarks form.
    /// @return Its session.
    [[nodiscard]] ::morph::forms::FormSession& importForm() noexcept { return _import; }

    /// @brief The idempotency key the import form currently carries. Tracked.
    /// @return A lower-case RFC 4122 UUID.
    [[nodiscard]] std::string const& importOpId() const { return _opId.get(); }

    /// @brief The status line's text. Tracked.
    /// @return The text, or empty.
    [[nodiscard]] std::string const& statusText() const { return _status.get().text; }

    /// @brief Whether the status line reports a failure. Tracked.
    /// @return True for a failure.
    [[nodiscard]] bool statusIsError() const { return _status.get().isError; }

    /// @brief Whether there is a status to show. Tracked.
    /// @return True when the text is non-empty.
    [[nodiscard]] bool hasStatus() const { return !_status.get().text.empty(); }

private:
    [[nodiscard]] std::optional<BookmarkId> loaded() const;
    void reportError(std::exception_ptr const& error);
    void onEdited();
    void onImported();
    void drawImportOpId();

    ::morph::reactive::Runtime* _rt;
    ::morph::bridge::BridgeHandler<BookmarkModel> _handler;
    std::function<bool()> _signedIn;
    std::function<void()> _onChanged;
    ::morph::reactive::Signal<std::optional<std::int64_t>> _openId;
    ::morph::reactive::Signal<StatusLine> _status;
    ::morph::reactive::Signal<std::string> _opId;
    // The bookmark the edit form was last prefilled from; 0 for none.
    std::int64_t _prefilledFor = 0;
    ::morph::reactive::Query<GetBookmark> _detail;
    ::morph::reactive::Mutation<ArchiveBookmark> _archive;
    ::morph::reactive::Mutation<UnarchiveBookmark> _unarchive;
    ::morph::reactive::Mutation<DeleteBookmark> _delete;
    ::morph::forms::FormSession _edit;
    ::morph::forms::FormSession _import;
    ::morph::reactive::Computed<std::string> _title;
    ::morph::reactive::Computed<std::vector<BookmarkFact>> _facts;
    ::morph::reactive::Computed<bool> _canAct;
    ::morph::reactive::Effect _onLoaded;
    ::morph::reactive::Effect _onDetailFailed;
    ::morph::reactive::Effect _onArchived;
    ::morph::reactive::Effect _onUnarchived;
    ::morph::reactive::Effect _onDeleted;
    ::morph::reactive::Effect _onArchiveFailed;
    ::morph::reactive::Effect _onUnarchiveFailed;
    ::morph::reactive::Effect _onDeleteFailed;
    ::morph::examples::FormSuccess _onEditSaved;
    // Last: it calls into everything above.
    ::morph::examples::FormSuccess _onImportDone;
};

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/controllers/bookmark_detail_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/bookmark_detail_controller.hpp"

#include <exception>
#include <glaze/glaze.hpp>
#include <morph/forms/engine/bridge_submitter.hpp>
#include <utility>

#include "app/uuid.hpp"
#include "controllers/forms_routing.hpp"

namespace bookmarks::client {

std::vector<BookmarkFact> factsOf(BookmarkView const& view) {
    auto const fact = [](std::string const& key, std::string const& value) {
        return BookmarkFact{.key = key, .line = key + ": " + value};
    };
    return {fact("url", view.url),
            fact("description", view.description),
            fact("notes", view.notes),
            fact("tags", joined(view.tags, ", ")),
            fact("visibility", visibilityText(view.visibility)),
            fact("read", readStateText(view.readState)),
            fact("archive", archiveStateText(view.archiveState)),
            fact("created", isoOrEmpty(view.createdAt)),
            fact("updated", isoOrEmpty(view.updatedAt))};
}

std::string editBodyOf(BookmarkView const& view) {
    EditBookmark const edit{.id = view.id,
                            .url = view.url,
                            .title = view.title,
                            .description = view.description,
                            .notes = view.notes,
                            .tags = view.tags,
                            .visibility = view.visibility};
    return glz::write_json(edit).value_or(std::string{"{}"});
}

std::string importBodyWith(std::string const& opId) {
    ImportBookmarks const action{.chunk = {}, .opId = ImportOpId{opId}};
    return glz::write_json(action).value_or(std::string{"{}"});
}

BookmarkDetailController::BookmarkDetailController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                                                   ::morph::exec::IExecutor& callbacks,
                                                   std::function<bool()> signedIn, std::function<void()> onChanged)
    : _rt{&runtime},
      _handler{bridge, &callbacks},
      _signedIn{std::move(signedIn)},
      _onChanged{std::move(onChanged)},
      _openId{runtime, std::nullopt},
      _status{runtime, StatusLine{}},
      _opId{runtime, std::string{}},
      _detail{runtime, _handler,
              [this]() -> std::optional<GetBookmark> {
                  auto const& openId = _openId.get();
                  if (!openId.has_value() || !_signedIn()) {
                      return std::nullopt;
                  }
                  return GetBookmark{.id = BookmarkId{*openId}};
              }},
      _archive{runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_detail}}},
      _unarchive{runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_detail}}},
      _delete{runtime, _handler},
      _edit{runtime, formModelFor("EditBookmark"), formsSubmitter(bridge, callbacks),
            ::morph::forms::bridgeChoiceFetcher(bridge, callbacks)},
      _import{runtime, formModelFor("ImportBookmarks"), formsSubmitter(bridge, callbacks),
              ::morph::forms::bridgeChoiceFetcher(bridge, callbacks)},
      _title{runtime,
             [this] {
                 auto const& view = _detail.value();
                 return view.has_value() ? headline(view->title, view->url)
                                         : std::string{"no bookmark open: activate one in the list"};
             }},
      _facts{runtime,
             [this] {
                 auto const& view = _detail.value();
                 return view.has_value() ? factsOf(*view) : std::vector<BookmarkFact>{};
             }},
      _canAct{runtime,
              [this] {
                  return _detail.value().has_value() && !_archive.pending() && !_unarchive.pending() &&
                         !_delete.pending();
              }},
      _onLoaded{runtime,
                [this] {
                    auto const& view = _detail.value();
                    if (!view.has_value() || !view->id.hasValue() || *view->id == _prefilledFor) {
                        return;
                    }
                    std::int64_t const loadedId = *view->id;
                    std::string const body = editBodyOf(*view);
                    _rt->untracked([&] {
                        _prefilledFor = loadedId;
                        _edit.prefill(body);
                    });
                }},
      _onDetailFailed{runtime,
                      [this] {
                          if (auto const error = _detail.error()) {
                              _status.set(failureLine(error));
                          }
                      }},
      _onArchived{runtime,
                  [this] {
                      if (_archive.lastResult().has_value()) {
                          _rt->untracked([this] {
                              _status.set(infoLine("archived"));
                              _onChanged();
                          });
                      }
                  }},
      _onUnarchived{runtime,
                    [this] {
                        if (_unarchive.lastResult().has_value()) {
                            _rt->untracked([this] {
                                _status.set(infoLine("unarchived"));
                                _onChanged();
                            });
                        }
                    }},
      _onDeleted{runtime,
                 [this] {
                     if (_delete.lastResult().has_value()) {
                         _rt->untracked([this] {
                             close();
                             _status.set(infoLine("deleted"));
                             _onChanged();
                         });
                     }
                 }},
      _onArchiveFailed{runtime, [this] { reportError(_archive.error()); }},
      _onUnarchiveFailed{runtime, [this] { reportError(_unarchive.error()); }},
      _onDeleteFailed{runtime, [this] { reportError(_delete.error()); }},
      _onEditSaved{runtime, _edit, [this] { onEdited(); }},
      _onImportDone{runtime, _import, [this] { onImported(); }} {
    drawImportOpId();
}

void BookmarkDetailController::open(std::int64_t bookmarkId) {
    if (_openId.peek() == std::optional<std::int64_t>{bookmarkId}) {
        _detail.refetch();
        return;
    }
    _status.set(StatusLine{});
    _openId.set(bookmarkId);
}

void BookmarkDetailController::close() {
    _openId.set(std::nullopt);
    _prefilledFor = 0;
}

std::optional<BookmarkId> BookmarkDetailController::loaded() const {
    auto const& view = _detail.value();
    if (!view.has_value() || !view->id.hasValue()) {
        return std::nullopt;
    }
    return view->id;
}

void BookmarkDetailController::reportError(std::exception_ptr const& error) {
    if (error != nullptr) {
        _status.set(failureLine(error));
    }
}

void BookmarkDetailController::archive() {
    if (auto const bookmark = loaded()) {
        _archive.run(ArchiveBookmark{.id = *bookmark});
    }
}

void BookmarkDetailController::unarchive() {
    if (auto const bookmark = loaded()) {
        _unarchive.run(UnarchiveBookmark{.id = *bookmark});
    }
}

void BookmarkDetailController::remove() {
    if (auto const bookmark = loaded()) {
        _delete.run(DeleteBookmark{.id = *bookmark});
    }
}

void BookmarkDetailController::onEdited() {
    _prefilledFor = 0;
    _status.set(infoLine("saved"));
    _detail.refetch();
    _onChanged();
}

void BookmarkDetailController::onImported() {
    ImportBookmarksResult result;
    auto const& reply = _import.lastReply();
    if (reply.has_value() && !glz::read_json(result, *reply)) {
        _status.set(infoLine("imported " + countText(result.imported) + ", skipped " + countText(result.skipped)));
    } else {
        _status.set(infoLine("imported"));
    }
    drawImportOpId();
    _onChanged();
}

void BookmarkDetailController::drawImportOpId() {
    std::string opId = ::morph::examples::newUuid();
    _import.prefill(importBodyWith(opId));
    _opId.set(std::move(opId));
}

}  // namespace bookmarks::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_bookmarks_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/bookmarks/ladder_bookmarks_tests "[bookmarks][controller]"
```

Expected: PASS. Mutation check: in `_onLoaded`, delete `|| *view->id == _prefilledFor`. Expected FAIL in "A
background refetch never overwrites what the user is typing into the edit form" (the refetch prefills again and
"Typing" is gone). Restore. Second: in `onImported()`, delete `drawImportOpId();`. Expected FAIL in "The import
form starts with a fresh opId…" (`importOpId() != firstOpId`). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/bookmarks/app/controllers examples/bookmarks/tests/client_fixture.hpp \
    examples/bookmarks/tests/test_bookmark_detail_controller.cpp
git commit -m "wip(bookmarks): the detail controller — open, archive, delete, edit, import

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 14: bookmarks' views

**Files:**
- Create: `examples/bookmarks/app/views/bookmark_views.{hpp,cpp}`
- Test: `examples/bookmarks/tests/test_bookmark_views.cpp`

**Interfaces:**
- Consumes: Part 2's `ui` builders (incl. `checkbox`, `spacer`, `switchOn<E>`), `ui::Mounted`, `RecordingBackend`
  (`find`, `all`, `prop`, `click`, `toggle`, `selectRows`, `activateRow`); `forms::formView`; the five
  controllers (Tasks 10–13).
- Produces: `bookmarks::client::{loginScreen, listPane, detailPane, tagsPane, feedPane, bookmarksScreen}`.

- [ ] **Step 1: Write the failing test**

Create `examples/bookmarks/tests/test_bookmark_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <string>

#include "client_fixture.hpp"
#include "controllers/session_controller.hpp"
#include "controllers/shared_feed_controller.hpp"
#include "controllers/tag_controller.hpp"
#include "testkit/db_fixture.hpp"
#include "views/bookmark_views.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using morph::ui::Key;
using morph::ui::Mounted;
using morph::ui::testing::RecordingBackend;
namespace client = bookmarks::client;

/// @brief A signed-in client with the four library controllers, wired as the application wires them.
struct LibraryFixture {
    LibraryFixture()
        : rig{bookmarks::testing::authedRig(Mode::Local)},
          runtime{*rig->executor()},
          feed{runtime, rig->bridge(0), *rig->executor(), [] { return true; }},
          tags{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [this] { list.refresh(); feed.refresh(); }},
          list{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [this] { tags.refresh(); feed.refresh(); }},
          detail{runtime, rig->bridge(0), *rig->executor(), [] { return true; }, [this] { list.refresh(); tags.refresh(); }} {}

    std::unique_ptr<BackendRig> rig;
    morph::reactive::Runtime runtime;
    client::SharedFeedController feed;
    client::TagController tags;
    client::BookmarkListController list;
    client::BookmarkDetailController detail;
};

}  // namespace

TEST_CASE("loginScreen: the tree, and its form signs in", "[bookmarks][view]") {
    DbFixture fixture;
    bookmarks::testing::ScopedTokenIssuer const issuer;
    BackendRig rig{Mode::Local, 1};
    morph::reactive::Runtime runtime{*rig.executor()};
    client::SessionController session{runtime, rig.bridge(0), *rig.executor()};
    RecordingBackend backend;
    Mounted const mounted{runtime, backend, client::loginScreen(session)};
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "Sign in").has_value(); }));
    auto const submit = backend.find("Button", "label", "Sign in");
    REQUIRE(submit.has_value());

    session.loginForm().prefill(R"({"username":"alice"})");
    REQUIRE(pumpUntil([&] { return backend.prop(*submit, "enabled") == "true"; }));
    backend.click(*submit);
    REQUIRE(pumpUntil([&] { return session.signedIn(); }));
}

TEST_CASE("listPane: the tree, the archived toggle, the selection and bulk archive", "[bookmarks][view]") {
    DbFixture fixture;
    LibraryFixture library;
    std::int64_t const first = bookmarks::testing::seedBookmark(*library.rig, "https://one.example");
    std::int64_t const second = bookmarks::testing::seedBookmark(*library.rig, "https://two.example");
    library.list.refresh();
    RecordingBackend backend;
    Mounted const mounted{library.runtime, backend, client::listPane(library.list, library.detail)};
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "2 bookmark(s)").has_value(); }));
    CHECK(backend.find("Button", "label", "Create bookmark").has_value());
    auto const bulk = backend.find("Button", "label", "Bulk archive");
    REQUIRE(bulk.has_value());
    CHECK(backend.prop(*bulk, "enabled") == "false");

    auto const tables = backend.all("Table");
    REQUIRE(tables.size() == 1);
    backend.selectRows(tables.front(), {Key{first}, Key{second}});
    REQUIRE(pumpUntil([&] { return backend.prop(*bulk, "enabled") == "true"; }));
    CHECK(backend.find("Text", "text", "2 selected").has_value());
    backend.click(*bulk);
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "0 bookmark(s)").has_value(); }));

    auto const archived = backend.find("Checkbox", "label", "show archived");
    REQUIRE(archived.has_value());
    backend.toggle(*archived);
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "2 bookmark(s)").has_value(); }));

    backend.activateRow(tables.front(), Key{first});
    REQUIRE(pumpUntil([&] { return library.detail.hasCurrent(); }));
}

TEST_CASE("detailPane: the tree, and its buttons drive the open bookmark", "[bookmarks][view]") {
    DbFixture fixture;
    LibraryFixture library;
    std::int64_t const bookmarkId = bookmarks::testing::seedBookmark(*library.rig, "https://open.example");
    RecordingBackend backend;
    Mounted const mounted{library.runtime, backend, client::detailPane(library.detail)};
    REQUIRE(pumpUntil([&] { return backend.find("Panel", "title", "no bookmark open: activate one in the list").has_value(); }));
    auto const archive = backend.find("Button", "label", "Archive");
    REQUIRE(archive.has_value());
    CHECK(backend.prop(*archive, "enabled") == "false");
    CHECK(backend.find("Button", "label", "Import").has_value());

    bookmarks::testing::openAndWait(library.detail, bookmarkId);
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "url: https://open.example").has_value(); }));
    CHECK(backend.find("Button", "label", "Save changes").has_value());
    backend.click(*archive);
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "archive: Archived").has_value(); }));
    CHECK(backend.find("Text", "text", "archived").has_value());
}

TEST_CASE("tagsPane: the tree, and its rename form renames", "[bookmarks][view]") {
    DbFixture fixture;
    LibraryFixture library;
    static_cast<void>(bookmarks::testing::seedBookmark(*library.rig, "https://one.example", {"work"}));
    library.tags.refresh();
    RecordingBackend backend;
    Mounted const mounted{library.runtime, backend, client::tagsPane(library.tags)};
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "work").has_value(); }));
    CHECK(backend.find("Panel", "title", "Tags (1)").has_value());
    auto const rename = backend.find("Button", "label", "Rename");
    REQUIRE(rename.has_value());
    CHECK(backend.find("Button", "label", "Merge").has_value());

    std::int64_t const tagId = library.tags.rows().front().tagId;
    library.tags.renameForm().prefill(R"({"id":)" + std::to_string(tagId) + R"(,"name":"office"})");
    REQUIRE(pumpUntil([&] { return backend.prop(*rename, "enabled") == "true"; }));
    backend.click(*rename);
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "office").has_value(); }));
}

TEST_CASE("feedPane: the tree shows shared bookmarks only", "[bookmarks][view]") {
    DbFixture fixture;
    LibraryFixture library;
    static_cast<void>(bookmarks::testing::seedBookmark(*library.rig, "https://private.example"));
    static_cast<void>(bookmarks::testing::seedBookmark(*library.rig, "https://shared.example", {},
                                                       bookmarks::Visibility::Shared));
    library.feed.refresh();
    RecordingBackend backend;
    Mounted const mounted{library.runtime, backend, client::feedPane(library.feed)};
    REQUIRE(pumpUntil([&] { return backend.find("Panel", "title", "Shared feed (1)").has_value(); }));
    CHECK(library.feed.rows().front().line.starts_with("https://shared.example · "));
    CHECK(backend.find("Text", "text", library.feed.rows().front().line).has_value());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_bookmarks_tests`
Expected: FAIL — `'views/bookmark_views.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/bookmarks/app/views/bookmark_views.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/bookmark_detail_controller.hpp"
#include "controllers/bookmark_list_controller.hpp"
#include "controllers/session_controller.hpp"
#include "controllers/shared_feed_controller.hpp"
#include "controllers/tag_controller.hpp"

/// @file
/// bookmarks' screens as view trees: bindings over the controllers, and nothing else. Every controller
/// passed in is borrowed and must outlive the mounted tree.

namespace bookmarks::client {

/// @brief The sign-in screen: the Login form.
/// @param session The sign-in controller.
/// @return The screen.
[[nodiscard]] ::morph::ui::Node loginScreen(SessionController& session);

/// @brief The collection: the create form, the archived toggle, the multi-select table and bulk archive.
///        Activating a row opens it in @p detail.
/// @param list The list controller.
/// @param detail The detail controller rows open into.
/// @return The pane.
[[nodiscard]] ::morph::ui::Node listPane(BookmarkListController& list, BookmarkDetailController& detail);

/// @brief The open bookmark: its facts, archive/unarchive/delete, the edit and import forms.
/// @param detail The detail controller.
/// @return The pane.
[[nodiscard]] ::morph::ui::Node detailPane(BookmarkDetailController& detail);

/// @brief The caller's tags, with the rename and merge forms.
/// @param tags The tags controller.
/// @return The pane.
[[nodiscard]] ::morph::ui::Node tagsPane(TagController& tags);

/// @brief The cross-user shared feed.
/// @param feed The feed controller.
/// @return The pane.
[[nodiscard]] ::morph::ui::Node feedPane(SharedFeedController& feed);

/// @brief The whole client: a header naming who is signed in, over the sign-in screen or the library.
/// @param session The sign-in controller; its `route()` picks the screen.
/// @param list The list controller.
/// @param detail The detail controller.
/// @param tags The tags controller.
/// @param feed The feed controller.
/// @return The root node.
[[nodiscard]] ::morph::ui::Node bookmarksScreen(SessionController& session, BookmarkListController& list,
                                                BookmarkDetailController& detail, TagController& tags,
                                                SharedFeedController& feed);

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/views/bookmark_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/bookmark_views.hpp"

#include <cstdint>
#include <morph/forms/engine/form_view.hpp>
#include <morph/reactive/signal.hpp>
#include <string>
#include <variant>
#include <vector>

namespace bookmarks::client {

namespace {

namespace ui = ::morph::ui;

[[nodiscard]] std::vector<ui::Key> keysOf(std::vector<std::int64_t> const& bookmarkIds) {
    std::vector<ui::Key> keys;
    keys.reserve(bookmarkIds.size());
    for (std::int64_t const bookmarkId : bookmarkIds) {
        keys.emplace_back(bookmarkId);
    }
    return keys;
}

[[nodiscard]] std::vector<std::int64_t> idsOf(std::vector<ui::Key> const& keys) {
    std::vector<std::int64_t> bookmarkIds;
    for (ui::Key const& key : keys) {
        if (auto const* bookmarkId = std::get_if<std::int64_t>(&key)) {
            bookmarkIds.push_back(*bookmarkId);
        }
    }
    return bookmarkIds;
}

[[nodiscard]] ui::Node form(::morph::forms::FormSession& session, std::string submitLabel) {
    return ::morph::forms::formView(session, {.overrides = {}, .gridColumns = 1, .submitLabel = std::move(submitLabel)});
}

template <class Controller>
[[nodiscard]] ui::Node statusOf(Controller& controller) {
    return ui::text({.text = [&controller] { return controller.statusText(); },
                     .role = [&controller] {
                         return controller.statusIsError() ? ui::TextRole::Error : ui::TextRole::Success;
                     },
                     .common = {.visible = [&controller] { return controller.hasStatus(); }}});
}

template <class Controller>
[[nodiscard]] ui::Node errorOf(Controller& controller) {
    return ui::text({.text = [&controller] { return controller.errorText(); },
                     .role = ui::TextRole::Error,
                     .common = {.visible = [&controller] { return controller.hasError(); }}});
}

[[nodiscard]] ui::Node bookmarkTable(BookmarkListController& list, BookmarkDetailController& detail) {
    return ui::table<BookmarkRow>(
        {ui::TableColumn{.label = "Bookmark", .width = ui::Sizing::stretch()}, ui::TableColumn{.label = "State"}},
        [&list] { return list.rows(); }, [](BookmarkRow const& row) { return ui::Key{row.bookmarkId}; },
        [](::morph::reactive::Signal<BookmarkRow> const& row) {
            return std::vector<ui::Node>{
                ui::text({.text = [&row] { return row.get().title; }}),
                ui::text({.text = [&row] { return row.get().state; }, .role = ui::TextRole::Muted})};
        },
        ui::TableOptions{
            .selectionMode = ui::SelectionMode::Multiple,
            .selection = [&list] { return keysOf(list.selection()); },
            .onSelectionChange = [&list](std::vector<ui::Key> const& keys) { list.select(idsOf(keys)); },
            .onActivate =
                [&detail](ui::Key const& key) {
                    if (auto const* bookmarkId = std::get_if<std::int64_t>(&key)) {
                        detail.open(*bookmarkId);
                    }
                },
            .common = {.layout = {.height = ui::Sizing::stretch()}}});
}

[[nodiscard]] ui::Node actionButton(BookmarkDetailController& detail, std::string label,
                                    void (BookmarkDetailController::*act)()) {
    return ui::button({.label = std::move(label),
                       .onClick = [&detail, act] { (detail.*act)(); },
                       .common = {.enabled = [&detail] { return detail.canAct(); }}});
}

}  // namespace

::morph::ui::Node loginScreen(SessionController& session) {
    return ui::column(
        {.children = {ui::text({.text = "Sign in", .role = ui::TextRole::Heading}),
                      ui::text({.text = "Dev-mode login: a username, no password. The token the server mints for "
                                        "it is real, server-signed and checked on every later action.",
                                .role = ui::TextRole::Muted}),
                      form(session.loginForm(), "Sign in")},
         .gap = 1});
}

::morph::ui::Node listPane(BookmarkListController& list, BookmarkDetailController& detail) {
    return ui::column(
        {.children = {statusOf(list),
                      ui::panel({.title = "New bookmark", .padding = 1, .child = form(list.createForm(), "Create bookmark")}),
                      ui::row({.children = {ui::button({.label = "Refresh", .onClick = [&list] { list.refreshAll(); }}),
                                            ui::checkbox({.label = "show archived",
                                                          .checked = [&list] { return list.includeArchived(); },
                                                          .onToggle = [&list](bool include) {
                                                              list.setIncludeArchived(include);
                                                          }}),
                                            ui::busy({.active = [&list] { return list.loading(); }, .label = "loading"}),
                                            ui::text({.text = [&list] { return list.countText(); },
                                                      .role = ui::TextRole::Muted})},
                               .gap = 2}),
                      bookmarkTable(list, detail),
                      ui::row({.children = {ui::text({.text = [&list] { return list.selectionText(); },
                                                      .role = ui::TextRole::Muted}),
                                            ui::button({.label = "Bulk archive",
                                                        .onClick = [&list] { list.bulkArchive(); },
                                                        .common = {.enabled = [&list] { return list.canBulkEdit(); }}}),
                                            ui::button({.label = "Bulk unarchive",
                                                        .onClick = [&list] { list.bulkUnarchive(); },
                                                        .common = {.enabled = [&list] { return list.canBulkEdit(); }}})},
                               .gap = 2})},
         .gap = 1,
         .common = {.layout = {.width = ui::Sizing::stretch()}}});
}

::morph::ui::Node detailPane(BookmarkDetailController& detail) {
    return ui::panel(
        {.title = [&detail] { return detail.detailTitle(); },
         .padding = 1,
         .child = ui::column(
             {.children =
                  {statusOf(detail),
                   ui::forEach<BookmarkFact>(
                       [&detail] { return detail.facts(); }, [](BookmarkFact const& fact) { return ui::Key{fact.key}; },
                       [](::morph::reactive::Signal<BookmarkFact> const& fact) {
                           return ui::text({.text = [&fact] { return fact.get().line; }});
                       }),
                   ui::row({.children = {actionButton(detail, "Archive", &BookmarkDetailController::archive),
                                         actionButton(detail, "Unarchive", &BookmarkDetailController::unarchive),
                                         actionButton(detail, "Delete", &BookmarkDetailController::remove)},
                            .gap = 2}),
                   ui::scroll(
                       {.child = ui::column(
                            {.children = {ui::panel({.title = "Edit",
                                                     .padding = 1,
                                                     .child = form(detail.editForm(), "Save changes"),
                                                     .common = {.visible = [&detail] { return detail.hasCurrent(); }}}),
                                          ui::panel({.title = "Import a Netscape bookmark file",
                                                     .padding = 1,
                                                     .child = form(detail.importForm(), "Import")})},
                             .gap = 1}),
                        .common = {.layout = {.height = ui::Sizing::stretch()}}})},
              .gap = 1}),
         .common = {.layout = {.width = ui::Sizing::stretch()}}});
}

::morph::ui::Node tagsPane(TagController& tags) {
    return ui::panel(
        {.title = [&tags] { return tags.heading(); },
         .padding = 1,
         .child = ui::column(
             {.children = {errorOf(tags),
                           ui::table<TagRow>(
                               {ui::TableColumn{.label = "#"}, ui::TableColumn{.label = "Tag", .width = ui::Sizing::stretch()},
                                ui::TableColumn{.label = "Bookmarks"}},
                               [&tags] { return tags.rows(); }, [](TagRow const& row) { return ui::Key{row.tagId}; },
                               [](::morph::reactive::Signal<TagRow> const& row) {
                                   return std::vector<ui::Node>{
                                       ui::text({.text = [&row] { return row.get().idText; }, .role = ui::TextRole::Muted}),
                                       ui::text({.text = [&row] { return row.get().name; }}),
                                       ui::text({.text = [&row] { return row.get().bookmarkCount; }})};
                               },
                               ui::TableOptions{}),
                           ui::panel({.title = "Rename a tag", .padding = 1, .child = form(tags.renameForm(), "Rename")}),
                           ui::panel({.title = "Merge two tags", .padding = 1, .child = form(tags.mergeForm(), "Merge")})},
              .gap = 1})});
}

::morph::ui::Node feedPane(SharedFeedController& feed) {
    return ui::panel({.title = [&feed] { return feed.heading(); },
                      .padding = 1,
                      .child = ui::column({.children = {errorOf(feed),
                                                        ui::forEach<FeedRow>(
                                                            [&feed] { return feed.rows(); },
                                                            [](FeedRow const& row) { return ui::Key{row.bookmarkId}; },
                                                            [](::morph::reactive::Signal<FeedRow> const& row) {
                                                                return ui::text({.text = [&row] { return row.get().line; }});
                                                            })},
                                           .gap = 1})});
}

::morph::ui::Node bookmarksScreen(SessionController& session, BookmarkListController& list,
                                  BookmarkDetailController& detail, TagController& tags, SharedFeedController& feed) {
    ui::Node const library = ui::row(
        {.children = {listPane(list, detail), detailPane(detail),
                      ui::column({.children = {tagsPane(tags), feedPane(feed)},
                                  .gap = 1,
                                  .common = {.layout = {.width = ui::Sizing::stretch()}}})},
         .gap = 2,
         .common = {.layout = {.height = ui::Sizing::stretch()}}});
    return ui::column(
        {.children = {ui::row({.children = {ui::text({.text = "bookmarks", .role = ui::TextRole::Heading}),
                                            ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                            ui::text({.text = [&session] { return session.principalText(); },
                                                      .role = ui::TextRole::Muted})},
                               .gap = 2}),
                      ui::switchOn<Route>([&session] { return session.route(); },
                                          {{Route::SignIn, loginScreen(session)}, {Route::Library, library}})},
         .gap = 1});
}

}  // namespace bookmarks::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_bookmarks_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/bookmarks/ladder_bookmarks_tests "[bookmarks][view]"
```

Expected: PASS. Mutation check: in `bookmarkTable`, change `.selectionMode = ui::SelectionMode::Multiple` to
`ui::SelectionMode::Single`. Expected FAIL in "listPane: the tree, the archived toggle, the selection and bulk
archive" (a single-selection table keeps one key, so "2 selected" never appears). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/bookmarks/app/views examples/bookmarks/tests/test_bookmark_views.cpp
git commit -m "wip(bookmarks): the sign-in, list, detail, tags and feed views

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 15: bookmarks' application, its one binary, and the old client deleted

**Files:**
- Create: `examples/bookmarks/app/bookmarks_application.{hpp,cpp}`, `examples/bookmarks/ui/main.cpp`
- Modify: `codecov.yml` — bookmarks' two `ignore:` entries
- Delete: `examples/bookmarks/gui/`, `examples/bookmarks/gui_lib/`, `examples/bookmarks/gui_wasm/`,
  `examples/bookmarks/tests/`: `test_bookmark_presenter.cpp`, `test_bookmark_qml_bridges.cpp`,
  `test_shared_feed_presenter.cpp`, `test_tag_presenter.cpp`, `test_gui_qml_smoke.cpp`
- Test: `examples/bookmarks/tests/smoke/test_bookmarks_frontends.cpp` (binary `ladder_bookmarks_smoke_tests`, C3)

**Interfaces:**
- Consumes: as Task 6, plus `bookmarks::db::setup`, `bookmarks::auth::setTokenIssuer`, `session::TokenIssuer`;
  the five controllers and `bookmarksScreen` (Tasks 10–14).
- Produces: `bookmarks::client::BookmarksApplication(ui::AppContext&, examples::AppEnvironment const&)`,
  `bookmarks::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&)` (the contract's convention:
  the local setup — the database from `env.db`, else `BOOKMARKS_DB`, else `bookmarks.db`, and the in-process
  token issuer — is built inside); the `bookmarks` binary.

Claims of the deleted test files, and where each one now lives:

| Deleted case | Re-expressed as |
|---|---|
| presenter: create then get (3 modes) | `test_bookmark_list_controller.cpp` "The create form stores a bookmark…" + `test_bookmark_detail_controller.cpp` "Opening a bookmark shows every fact" |
| presenter: edit (3 modes) | "The edit form is prefilled from the open bookmark, and a save shows on reload" |
| presenter: archive then unarchive (3 modes) | "Archive then unarchive the open bookmark" |
| presenter: remove, then get fails (3 modes) | "Delete closes the detail, and opening it again fails" |
| presenter: list returns what was created (3 modes) | "The create form stores a bookmark…" |
| presenter: getChangesSince (3 modes) | dropped — no screen asks for changes (README, client gaps); `test_bookmark_model.cpp` "GetChangesSince returns only bookmarks touched after the given instant" and its two neighbours |
| presenter: bulkEdit (3 modes) | "Bulk archive and unarchive act on the selection and clear it" |
| presenter: importChunk then exportAll (3 modes) | import: "The import form starts with a fresh opId…"; export: dropped — no screen exports; `test_bookmark_model.cpp` "ExportBookmarks emits every owned bookmark as a Netscape file, and it re-imports" |
| presenter: every validation-driven action routes its failure | create: "The create form refuses an empty url…"; edit/import: their forms are not ready without an id/chunk (the detail case's `edit.body()` wait); archive/unarchive/delete/get with no id: "…with nothing open the actions issue nothing"; bulk with no ids: "With nothing selected the bulk actions issue nothing" |
| presenter: get unknown id | "An unknown id reports the model's message…" |
| presenter: list/changes/export with no session | "Signed out the list is idle; with no session at all it reports, not crashes" |
| tag presenter: list (3 modes) | `test_tag_and_feed_controllers.cpp` "TagController lists every tag the caller owns…" |
| tag presenter: rename (3 modes) | "TagController renames a tag through its form…" |
| tag presenter: merge (3 modes) | "TagController merges two tags through its form" |
| tag presenter: every action routes its failure | "The tag forms show the model's refusal, and nothing changes" |
| tag presenter: list with no session | "Signed out, the tag and feed queries are idle…" |
| feed presenter: shared only (3 modes) | "SharedFeedController lists every shared bookmark and never a private one" |
| feed presenter: no session | "Signed out, the tag and feed queries are idle…" |
| bridges: QML surface audit | dropped with the QML (spec 4 §7) |
| bridges: metaobject facts (CONSTANT schemas, six schemas) | `test_forms_routing.cpp` "Every one of the six forms reads its schema and submits explicitly" |
| bridges: every form opts out of auto-submit | same case |
| bridges: a read-only action carries no x-submitMode | same case (`ListBookmarks` is `Automatic`) |
| bridges: the open bag carries every key | "Opening a bookmark shows every fact" |
| bridges: rows in the narrower shape, no notes | "The create form stores a bookmark…" (+ `static_assert(!CarriesNotes<BookmarkRow>)`) |
| bridges: tag rows {id, name, count} | "TagController lists every tag…" |
| bridges: feed rows, Shared only | "SharedFeedController lists every shared bookmark…" (+ `static_assert(!CarriesNotes<FeedRow>)`) |
| bridges: second arm of visibility/archive renderers | "Archive then unarchive the open bookmark" (Shared, Archived) + `test_bookmark_text.cpp` |
| bridges: bulkArchive true/false → Archive/Unarchive | "Bulk archive and unarchive act on the selection and clear it" |
| bridges: the six actions route to their models | "loginSubmitter routes each of the six form actions to the model that serves it" |
| bridges: an unrouted action type is reported | "formsSubmitter refuses an action outside the six, naming it…" |
| bridges: destroyed with a submit in flight | "a bookmark list destroyed with a create in flight drops its reply" |
| bridges: token installed, loggedIn before replyReceived | "A Login reply is redacted, and the session is installed before it shows" |
| bridges: decodeLoginResult accepts/rejects | "decodeLoginResult accepts a real Login reply and rejects anything that is not one" |
| bridges: decodeLoginResult reads back a redacted reply | "A Login reply is redacted…" |
| smoke: Main.qml loads; the post-login screen loads | `tests/smoke/test_bookmarks_frontends.cpp` (TUI and Qt Quick) and `test_bookmark_views.cpp` (each screen) |

- [ ] **Step 1: Write the failing test**

Create `examples/bookmarks/tests/smoke/test_bookmarks_frontends.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"
#include "bookmarks_application.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/frontend_smoke.hpp"

#if MORPH_EXAMPLE_HAS_TUI || MORPH_EXAMPLE_HAS_QT_QUICK

namespace {

/// @brief The application in local mode, `--db` naming the database `DbFixture` already prepared.
[[nodiscard]] morph::ui::ApplicationFactory bookmarksFactory() {
    return [](morph::ui::AppContext& ctx) {
        morph::examples::AppEnvironment env;
        env.db = morph::ladder::testkit::DbFixture::computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"));
        return bookmarks::client::makeApplication(ctx, env);
    };
}

}  // namespace

#endif

#if MORPH_EXAMPLE_HAS_TUI
TEST_CASE("bookmarks mounts and quits on the TUI", "[bookmarks][frontend]") {
    morph::ladder::testkit::DbFixture const fixture;
    morph::examples::testing::runFrontendSmoke(bookmarksFactory(), morph::examples::testing::SmokeFrontend::Tui);
}
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
TEST_CASE("bookmarks mounts and quits on Qt Quick", "[bookmarks][frontend]") {
    morph::ladder::testkit::DbFixture const fixture;
    morph::examples::testing::runFrontendSmoke(bookmarksFactory(),
                                               morph::examples::testing::SmokeFrontend::QtQuick);
}
#endif
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_bookmarks_smoke_tests`
Expected: FAIL — `'bookmarks_application.hpp' file not found`.

- [ ] **Step 3: Implement the application and the binary**

Create `examples/bookmarks/app/bookmarks_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>

#include "app/app_environment.hpp"
#include "app/transport.hpp"
#include "controllers/bookmark_detail_controller.hpp"
#include "controllers/bookmark_list_controller.hpp"
#include "controllers/session_controller.hpp"
#include "controllers/shared_feed_controller.hpp"
#include "controllers/tag_controller.hpp"

/// @file
/// bookmarks as a `ui::Application`: one connection, the sign-in controller, and the four library panes.

namespace bookmarks::client {

/// @brief bookmarks' application.
///
/// Every library query is keyed on the session controller's `signedIn()`, so the sign-in screen issues
/// nothing but the Login it submits. A change one pane makes refreshes the panes that show it too: a
/// rename shows in the list, the open bookmark and the feed; a create or a bulk edit in the tags and the
/// feed. Members die detail first and the connection last.
class BookmarksApplication final : public ::morph::ui::Application {
public:
    /// @param ctx The frontend's context.
    /// @param env What the command line (or the browser build) asked for; without `--server` its `db` (else
    ///            `BOOKMARKS_DB`, else `bookmarks.db` in the working directory) is the database set up and hosted.
    BookmarksApplication(::morph::ui::AppContext& ctx, ::morph::examples::AppEnvironment const& env);

    ~BookmarksApplication() override = default;
    BookmarksApplication(BookmarksApplication const&) = delete;
    BookmarksApplication& operator=(BookmarksApplication const&) = delete;
    BookmarksApplication(BookmarksApplication&&) = delete;
    BookmarksApplication& operator=(BookmarksApplication&&) = delete;

    /// @brief The client's root view.
    /// @return `bookmarksScreen()` over the controllers.
    [[nodiscard]] ::morph::ui::Node view() override;

private:
    std::unique_ptr<::morph::examples::Connection> _connection;
    SessionController _session;
    SharedFeedController _feed;
    TagController _tags;
    BookmarkListController _list;
    BookmarkDetailController _detail;
};

/// @brief The application factory `ui/main.cpp` and the smoke tests share.
/// @param ctx The frontend's context.
/// @param env The environment.
/// @return The application.
[[nodiscard]] std::unique_ptr<::morph::ui::Application> makeApplication(::morph::ui::AppContext& ctx,
                                                                       ::morph::examples::AppEnvironment const& env);

}  // namespace bookmarks::client
```

Create `examples/bookmarks/app/bookmarks_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks_application.hpp"

#include <cstdlib>
#include <memory>
#include <string>

#include "views/bookmark_views.hpp"

#ifndef __EMSCRIPTEN__
#include <morph/session/session_auth.hpp>

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/db/database.hpp"
#endif

namespace bookmarks::client {

namespace {

/// @brief How a client without `--server` prepares itself: the database from `--db`, else `BOOKMARKS_DB`, else
///        `bookmarks.db`; and a token issuer, because `AuthModel` mints Login tokens in-process and a local
///        deployment has no server secret. A local client has no authorizer either, so it is single-user by
///        construction. The browser build is always remote and never calls it.
[[nodiscard]] ::morph::examples::LocalSetup localSetup() {
#ifdef __EMSCRIPTEN__
    return ::morph::examples::LocalSetup{};
#else
    return ::morph::examples::LocalSetup{.setupDatabase = [](std::string const& database) {
        std::string connection = database;
        if (connection.empty()) {
            char const* fromEnvironment = std::getenv("BOOKMARKS_DB");
            connection = fromEnvironment != nullptr ? fromEnvironment
                                                    : "DRIVER=SQLite3;Database=bookmarks.db;Timeout=5000";
        }
        db::setup(connection);
        auth::setTokenIssuer(std::make_shared<::morph::session::TokenIssuer>(
            std::string{"local-mode-development-secret"}, ::morph::session::hmacSha256));
    }};
#endif
}

}  // namespace

BookmarksApplication::BookmarksApplication(::morph::ui::AppContext& ctx, ::morph::examples::AppEnvironment const& env)
    : _connection{::morph::examples::connect(ctx, env, localSetup())},
      _session{ctx.runtime(), _connection->bridge(), _connection->callbacks()},
      _feed{ctx.runtime(), _connection->bridge(), _connection->callbacks(), [this] { return _session.signedIn(); }},
      _tags{ctx.runtime(), _connection->bridge(), _connection->callbacks(), [this] { return _session.signedIn(); },
            [this] {
                _list.refresh();
                _detail.refresh();
                _feed.refresh();
            }},
      _list{ctx.runtime(), _connection->bridge(), _connection->callbacks(), [this] { return _session.signedIn(); },
            [this] {
                _tags.refresh();
                _detail.refresh();
                _feed.refresh();
            }},
      _detail{ctx.runtime(), _connection->bridge(), _connection->callbacks(), [this] { return _session.signedIn(); },
              [this] {
                  _list.refresh();
                  _tags.refresh();
                  _feed.refresh();
              }} {}

::morph::ui::Node BookmarksApplication::view() { return bookmarksScreen(_session, _list, _detail, _tags, _feed); }

std::unique_ptr<::morph::ui::Application> makeApplication(::morph::ui::AppContext& ctx,
                                                          ::morph::examples::AppEnvironment const& env) {
    return std::make_unique<BookmarksApplication>(ctx, env);
}

}  // namespace bookmarks::client
```

Create `examples/bookmarks/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

/// @file
/// bookmarks' one binary, native and WebAssembly: it reads the environment, offers every frontend this
/// build has, lets `ui::selectFrontend` pick one, and hands the frontend the application factory.
///
/// @code
/// bookmarks                                  # in-process: hosts the models over --db / BOOKMARKS_DB
/// bookmarks --server ws://127.0.0.1:8766     # against ladder_bookmarks_server
/// bookmarks --ui=tui                         # force the terminal UI
/// @endcode
///
/// The browser build is always remote, against the url baked in as `MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL` (a
/// `?server=` in the page url wins).

#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <string>
#include <vector>

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#endif
#if MORPH_EXAMPLE_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

#include "app/app_environment.hpp"
#include "bookmarks_application.hpp"

int main(int argc, char** argv) {
    try {
        auto env = morph::examples::AppEnvironment::fromArgs(argc, argv);
#ifdef __EMSCRIPTEN__
        if (!env.server.has_value()) {
            env.server = std::string{MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL};
        }
#endif
        std::vector<morph::ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
        built.push_back(morph::qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
        built.push_back(morph::tui::frontendOption());
#endif
        auto const frontend = morph::ui::selectFrontend(built, argc, argv);
        return frontend->run(
            [&env](morph::ui::AppContext& ctx) { return bookmarks::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "bookmarks: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "bookmarks: unknown error\n";
    }
    return 1;
}
```

- [ ] **Step 4: Delete the old client, its tests, and its coverage exclusions**

```bash
git rm -r -q examples/bookmarks/gui examples/bookmarks/gui_lib examples/bookmarks/gui_wasm \
    examples/bookmarks/tests/test_bookmark_presenter.cpp examples/bookmarks/tests/test_bookmark_qml_bridges.cpp \
    examples/bookmarks/tests/test_shared_feed_presenter.cpp examples/bookmarks/tests/test_tag_presenter.cpp \
    examples/bookmarks/tests/test_gui_qml_smoke.cpp
```

In `codecov.yml`, replace the `ignore:` entries `"examples/bookmarks/gui/**"` and `"examples/bookmarks/gui_wasm/**"`
with the one entry `"examples/bookmarks/ui/**"`.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build/all --target ladder_bookmarks_tests ladder_bookmarks_smoke_tests bookmarks ladder_bookmarks_server
QT_QPA_PLATFORM=offscreen ./build/all/examples/bookmarks/ladder_bookmarks_tests "[bookmarks]"
ctest --test-dir build/all -R '^bookmarks\.smoke\.' --output-on-failure
ls examples/bookmarks    # CMakeLists.txt README.md app include src tests ui
```

Expected: PASS; the `ctest` line runs both frontend cases. Mutation check: change
`BookmarksApplication::view()`'s body to `throw std::logic_error{"no view"};` (with `#include <stdexcept>`).
Expected FAIL in both `bookmarks.smoke.` cases. Restore.

- [ ] **Step 6: Commit**

```bash
git add -A examples/bookmarks codecov.yml
git commit -m "wip(bookmarks): the application, one binary for every frontend, the QML client removed

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 16: bookmarks' README

**Files:**
- Modify: `examples/bookmarks/README.md` — "Running it", one Definition-of-done bullet, the first-page gap, and
  the section "The client, and its known gaps — stated rather than smoothed over" up to its subsection
  "Two bugs the first real client run found" (kept as it is)

**Interfaces:** none.

- [ ] **Step 1: List the stale references**

```bash
grep -n 'gui_lib\|gui_wasm\|ladder_bookmarks_gui\|gui/main\.cpp\|gui/qml\|DynamicForm\|BookmarkBridge\|FormsBridge\|QML binding\|presenter level' examples/bookmarks/README.md
```

Expected: the lines the next step replaces (31–33, 386, 433–434, 442–533).

- [ ] **Step 2: Rewrite**

Replace the "Running it" code block with:

````markdown
```bash
# One-time configure (Qt 6.5+ and an ODBC SQLite3 driver; the client needs at
# least one frontend, the terminal UI or Qt Quick):
cmake -S . -B build -G Ninja \
    -DMORPH_BUILD_QT=ON -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON \
    -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=bookmarks

# Server (owns the database, the signing secret, the action journal, the
# metadata-fetch worker and the outbox relay). The secret is required and has
# no default: it signs every token the server mints and verifies every token
# it is shown, so a built-in fallback would be a published signing key.
BOOKMARKS_TOKEN_SECRET="pick-something-real" \
BOOKMARKS_DB="DRIVER=SQLite3;Database=bookmarks.db;Timeout=5000" \
BOOKMARKS_PORT=8766 ./build/examples/bookmarks/ladder_bookmarks_server

# The client, either deployment mode, on whichever frontend suits:
./build/examples/bookmarks/bookmarks                                  # in-process
./build/examples/bookmarks/bookmarks --server ws://127.0.0.1:8766
./build/examples/bookmarks/bookmarks --ui=tui                         # force the terminal UI
```
````

In "Definition of done" (a `\n` in a quoted original marks a line break in the file), replace "This criterion is
met at the model and presenter level, where\n  `GetChangesSince`
is implemented, tested and exposed." with "This criterion is met at the model level, where\n  `GetChangesSince` is
implemented and tested."

Replace the Known-gaps bullet "**The GUI never leaves the first page.**" with:

```markdown
- **The client never leaves the first page.** `BookmarkListController` and `SharedFeedController` query the
  first page and nothing asks for a further one, so the client shows at most the first ~20 bookmarks (and the
  first ~20 shared-feed entries). Pagination is fully implemented and tested at the model level — the keyset
  cursor works — it is only the client that does not use it.
```

Replace the section body from "## The client, and its known gaps — stated rather than smoothed over" down to (not
including) "### Two bugs the first real client run found" with:

```markdown
## The client, and its known gaps — stated rather than smoothed over

The client (`app/`, target `ladder_bookmarks_app`) is toolkit-free and schema-driven throughout
(`../IMPLEMENTATION.md` rule 2). `Login`, `CreateBookmark`, `EditBookmark`, `ImportBookmarks`, `RenameTag` and
`MergeTags` are runtime forms (`forms::FormSession` over the schema each action emits), rendered by
`forms::formView` on whichever frontend runs — including the sign-in screen: there is **no hand-built username
field**. Each of the six declares `explicitSubmit = true`, so each form has its own gated Submit button.
`controllers/forms_routing.cpp` routes the six across `AuthModel`, `BookmarkModel` and `TagModel` through the
bridge's registry and refuses any other action type by name. The one non-form input on the screen is the
table's multiple selection, which types nothing; it drives `BulkEdit` through `BookmarkListController`.

One piece of glue carries its own written justification, per rule 2's "(b) pure glue with no domain logic"
clause:

- `loginSubmitter` installs the token the server returned as the bridge's default session — before the Login
  reply resolves, so every later call carries it — and hands the form a copy of the reply with the token
  removed: a bearer credential has no reason to reach anything a view can show. It decides nothing; the token
  and the principal are the server's.

What the client shows differently from the QML client it replaced:

- The collection is a table with multiple selection (bookmark, state) instead of a list with a checkbox per
  row; activating a row (Enter, a double click) opens it in the detail pane.
- The edit form starts from the open bookmark — its id and fields prefilled — instead of empty; it is hidden
  while nothing is open. A background refresh does not overwrite what is being typed.
- The import form carries a fresh UUID as its `opId`, drawn again after every import, instead of asking for one.
- Each pane reports its own outcome (the list, the detail pane) rather than one status line for the window; a
  form shows its own reply or error.
- A tag's id is its own table column, so the rename and merge forms' ids can be read off it.

Known gaps:

- **Ids are typed into the rename and merge forms.** `RenameTag`/`MergeTags` carry plain `TagId`s, not
  `morph::forms::Choice` fields, so the forms ask for numbers rather than offering the tags to pick from.
- **The client never polls for background-job results.** The metadata worker fills a title in some seconds
  after a bookmark is created, and `GetChangesSince` is the action a client asks for that with — implemented
  and tested in `BookmarkModel`, dispatched by no controller. A fetched title appears on the next **Refresh**.
- **At least four model instances per client — counted from the code, not measured against a server.** Each
  querying controller owns a handler: one `SharedFeedModel`, one `TagModel`, two `BookmarkModel` (list and
  detail); the forms' routing may register more for `AuthModel`, `BookmarkModel` and `TagModel`. `app.cpp`'s
  `kMaxLiveModels` comment budgets "roughly one instance per model type it uses (four in this rung)", so the
  256 cap may cover fewer concurrent clients than that comment assumes.
- **Registration timing.** The library queries fire as soon as the session is installed. A call made before
  its handler's registration round trip lands waits for it, or is rejected if registration fails. Remote mode
  has no connect timeout, so a server that never answers leaves those calls pending.
- **No `--seed`.** `LADDER.md` asks every rung for one; this rung's server ships none, deliberately — see
  `src/server/main.cpp`'s file comment for the argument. Demo data is created through the client.
- **The frontend smoke tests prove mounting, not behaviour** — the behavioural half is the controller and view
  suites plus a manual end-to-end run.
```

- [ ] **Step 3: Verify**

Re-run Step 1's `grep`. Expected: no output. Then markdownlint on the file. Expected: no findings.

- [ ] **Step 4: Commit**

```bash
git add examples/bookmarks/README.md
git commit -m "wip(bookmarks): README describes the new client

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 17: Verify bookmarks, and squash its group

**Files:** none new.

- [ ] **Step 1: The rung's suite**

```bash
cmake --build build/all
ctest --test-dir build/all -L 'ladder-bookmarks|ladder-pastebin|ladder-0' --output-on-failure
```

Expected: every test passes.

- [ ] **Step 2: The client is toolkit-free**

```bash
grep -rnE '#include <Q|morph/(qt|tui|qt_quick)/' examples/bookmarks/app && echo "toolkit include in app/" || true
for t in ladder_bookmarks_app ladder_bookmarks_lib; do
  own=$(ninja -C build/all -t commands "$t" | grep "CMakeFiles/$t.dir/" || true)
  m=$(printf '%s\n' "$own" | grep -c . || true)
  n=$(printf '%s\n' "$own" | grep -cE 'Qt6|QtCore|/qt/' || true)
  echo "$t: $m own compile lines, $n Qt-bearing"; test "$m" -gt 0 && test "$n" -eq 0
done
```

Expected: no `grep` output; both targets `0`.

- [ ] **Step 3: The model tests are untouched**

```bash
git diff --exit-code master -- examples/bookmarks/tests/test_app.cpp examples/bookmarks/tests/test_bookmark_dto.cpp \
    examples/bookmarks/tests/test_bookmark_model.cpp examples/bookmarks/tests/test_bookmarks_authorizer.cpp \
    examples/bookmarks/tests/test_bookmarks_payload_shape.cpp examples/bookmarks/tests/test_bookmarks_schema.cpp \
    examples/bookmarks/tests/test_bookmarks_types.cpp examples/bookmarks/tests/test_netscape_bookmarks.cpp \
    examples/bookmarks/tests/test_shared_feed_model.cpp examples/bookmarks/tests/test_tag_bulk_dto.cpp \
    examples/bookmarks/tests/test_tag_model.cpp examples/bookmarks/tests/.clang-tidy
```

Expected: exit 0.

- [ ] **Step 4: Run the application by hand**

```bash
BOOKMARKS_TOKEN_SECRET=local-check BOOKMARKS_PORT=8766 ./build/all/examples/bookmarks/ladder_bookmarks_server &
./build/all/examples/bookmarks/bookmarks --server ws://127.0.0.1:8766 --ui=tui      # as alice
./build/all/examples/bookmarks/bookmarks --server ws://127.0.0.1:8766 --ui=qt       # as bob
kill %1
```

Sign in as two users; create a private and a shared bookmark as alice and see only the shared one in bob's
feed; select two bookmarks and bulk-archive them; rename a tag and see the list follow. Record what was
exercised in the squash body.

- [ ] **Step 5: Sanitizer**

```bash
cmake --preset clang-asan -DMORPH_BUILD_QT=ON -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON \
      -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=bookmarks
cmake --build build/clang-asan --target ladder_bookmarks_tests
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/examples/bookmarks/ladder_bookmarks_tests asan
QT_QPA_PLATFORM=offscreen ./build/clang-asan/examples/bookmarks/ladder_bookmarks_tests "[bookmarks][controller],[bookmarks][view]"
```

Expected: clean.

- [ ] **Step 6: clang-tidy over the changed lines** — CONTRIBUTING's recipe, file count printed and asserted.
  Expected: no findings in `examples/bookmarks/app/`, `ui/`, `tests/`.

- [ ] **Step 7: Commit any fixes** (skip if none, and say so)

```bash
git add -A examples/bookmarks
git commit -m "wip(bookmarks): fixes from the verification gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

- [ ] **Step 8: Squash the group**

Follow the master plan's "Squashing a part" procedure with `key=bookmarks` and this message:

```text
examples/bookmarks: one app, any frontend

bookmarks' client is a toolkit-free app library: a sign-in controller whose
Login installs the session before its (token-free) reply shows, list,
detail, tags and shared-feed controllers keyed on that session, and six
runtime forms routed across AuthModel, BookmarkModel and TagModel; the list
is a multi-select table with bulk archive. One binary, ui/main.cpp, runs it
on the TUI or Qt Quick, native or in the browser. The server's App moved to
ladder_bookmarks_server_app; the QML client and its tests are gone.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure must show the history ending in `examples/pastebin: …`, `examples/bookmarks: …`.

---
## Group 3 — polls (key `polls`)

polls' `App` is already Qt-free (`include/polls/app/app.hpp` names no Qt type), so it stays in
`ladder_polls_lib`; the domain library loses its `Qt6::Core` link through Task 1's `app/` rule the moment
Task 18 creates `examples/polls/app/`.

### Task 18: The landing and create-poll controllers — option drafts as a signal

**Files:**
- Modify: `examples/polls/CMakeLists.txt` — whole file
- Create: `examples/polls/app/controllers/poll_text.{hpp,cpp}`,
  `examples/polls/app/controllers/landing_controller.{hpp,cpp}`,
  `examples/polls/app/controllers/create_poll_controller.{hpp,cpp}`
- Create: `examples/polls/tests/client_fixture.hpp`
- Test: `examples/polls/tests/test_create_poll_controller.cpp`

**Interfaces:**
- Consumes: `reactive::{Signal::mutate, Mutation, Computed, Effect, errorMessage}`;
  `polls::{PollModel, CreatePoll, CreatePollOption, CreatePollResult, VoteChoice, Count, kMinOptions, kMaxOptions}`
  (`polls/dto/poll_dto.hpp`, `polls/core/types.hpp`); `BackendRig(Mode, std::size_t, std::shared_ptr<IAuthorizer>)`;
  `polls::auth::PollsAuthorizer`.
- Produces: `polls::client::{StatusLine, trimmed, countText, choiceName, choiceNamed, choiceLabel, infoLine,
  failureLine, CreateAccess, LandingController, OptionDraft, CreatePollController}`;
  `LandingController(reactive::Runtime&)` with `pollIdDraft()`, `setPollIdDraft()`, `canOpen()`,
  `trimmedPollId()`; `CreatePollController(Runtime&, Bridge&, IExecutor&)` with `title()`, `setTitle()`,
  `options() -> reactive::Signal<std::vector<OptionDraft>> const&`, `addOption()`, `removeOption()`,
  `setOptionLabel()`, `canAdd()`, `canRemove()`, `canSubmit()`, `submit()`, `created()`, `editing()`, `pollId()`,
  `adminToken()`, `participantToken()`, `startOver()`, `statusText()`, `statusIsError()`, `hasStatus()`;
  `polls::testing::{pollsRig, seedPoll}`.

- [ ] **Step 1: Write the failing test**

Create `examples/polls/tests/client_fixture.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "polls/auth/polls_authorizer.hpp"
#include "polls/models/poll_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

/// @file
/// The wiring polls' client tests share.

namespace polls::testing {

/// @brief A rig with the rung's authorizer installed (it matters in Socket mode).
/// @param mode The deployment shape.
/// @return The rig.
[[nodiscard]] inline std::unique_ptr<::morph::ladder::testkit::BackendRig> pollsRig(::morph::ladder::testkit::Mode mode) {
    return std::make_unique<::morph::ladder::testkit::BackendRig>(mode, 1, std::make_shared<auth::PollsAuthorizer>());
}

/// @brief Creates a poll through a plain handler of the rig's own, outside any controller.
/// @param rig The rig.
/// @param title The poll's title.
/// @param labels Its option labels.
/// @return The new poll's ids and tokens.
[[nodiscard]] inline CreatePollResult seedPoll(::morph::ladder::testkit::BackendRig& rig, std::string title,
                                               std::vector<std::string> labels) {
    auto creator = rig.client<PollModel>(0);
    CreatePoll create{.title = std::move(title), .options = {}};
    for (auto& label : labels) {
        create.options.push_back(CreatePollOption{.label = std::move(label)});
    }
    return ::morph::ladder::testkit::awaitQt(creator.execute(create));
}

}  // namespace polls::testing
```

Create `examples/polls/tests/test_create_poll_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <string>
#include <vector>

#include "client_fixture.hpp"
#include "controllers/create_poll_controller.hpp"
#include "controllers/landing_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using polls::client::CreatePollController;

[[nodiscard]] std::vector<std::int64_t> keysOf(CreatePollController const& create) {
    std::vector<std::int64_t> keys;
    for (auto const& draft : create.options().peek()) {
        keys.push_back(draft.key);
    }
    return keys;
}

[[nodiscard]] std::string labelOf(CreatePollController const& create, std::int64_t key) {
    for (auto const& draft : create.options().peek()) {
        if (draft.key == key) {
            return draft.label;
        }
    }
    return "<absent>";
}

}  // namespace

TEST_CASE("option drafts: the bounds hold and a removal keeps the other drafts' keys", "[polls][controller]") {
    DbFixture fixture;
    auto rig = polls::testing::pollsRig(Mode::Local);
    morph::reactive::Runtime runtime{*rig->executor()};
    CreatePollController create{runtime, rig->bridge(0), *rig->executor()};

    auto const initial = keysOf(create);
    REQUIRE(initial.size() == polls::kMinOptions);
    CHECK_FALSE(create.canRemove());
    create.removeOption(initial.front());
    CHECK(keysOf(create) == initial);  // refused at the minimum

    while (create.canAdd()) {
        create.addOption();
    }
    REQUIRE(create.options().peek().size() == polls::kMaxOptions);
    create.addOption();
    CHECK(create.options().peek().size() == polls::kMaxOptions);  // refused at the maximum

    auto const full = keysOf(create);
    create.setOptionLabel(full[5], "kept");
    create.removeOption(full[3]);
    auto expected = full;
    expected.erase(expected.begin() + 3);
    CHECK(keysOf(create) == expected);  // every other draft keeps its key, so its row keeps its widgets
    CHECK(labelOf(create, full[5]) == "kept");

    create.addOption();
    CHECK(keysOf(create).back() > *std::ranges::max_element(full));  // a key is never reused
}

TEST_CASE("canSubmit needs a title and every option label, and submit() refuses otherwise", "[polls][controller]") {
    DbFixture fixture;
    auto rig = polls::testing::pollsRig(Mode::Local);
    morph::reactive::Runtime runtime{*rig->executor()};
    CreatePollController create{runtime, rig->bridge(0), *rig->executor()};
    auto const keys = keysOf(create);

    CHECK_FALSE(create.canSubmit());
    create.setTitle("   ");
    create.setOptionLabel(keys[0], "2026-09-01");
    create.setOptionLabel(keys[1], "2026-09-02");
    CHECK_FALSE(create.canSubmit());  // a blank title
    create.setTitle("Team offsite");
    create.setOptionLabel(keys[1], "  ");
    CHECK_FALSE(create.canSubmit());  // a blank option
    create.submit();
    CHECK_FALSE(create.created());
}

TEST_CASE("The create screen creates a poll and shows its three ids, all three backend modes", "[polls][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = polls::testing::pollsRig(mode);
    morph::reactive::Runtime runtime{*rig->executor()};
    CreatePollController create{runtime, rig->bridge(0), *rig->executor()};
    auto const keys = keysOf(create);
    create.setTitle("Team offsite");
    create.setOptionLabel(keys[0], "2026-09-01");
    create.setOptionLabel(keys[1], "2026-09-02");
    REQUIRE(create.canSubmit());

    create.submit();
    REQUIRE(pumpUntil([&] { return create.created(); }));
    CHECK_FALSE(create.pollId().empty());
    CHECK_FALSE(create.adminToken().empty());
    CHECK_FALSE(create.participantToken().empty());
    CHECK(create.adminToken() != create.participantToken());
    CHECK(create.adminToken() != create.pollId());
    CHECK_FALSE(create.statusIsError());

    create.startOver();
    CHECK_FALSE(create.created());
    CHECK(create.title().empty());
    CHECK(create.options().peek().size() == polls::kMinOptions);
}

TEST_CASE("LandingController opens only a non-blank id, trimmed", "[polls][controller]") {
    DbFixture fixture;
    auto rig = polls::testing::pollsRig(Mode::Local);
    morph::reactive::Runtime runtime{*rig->executor()};
    polls::client::LandingController landing{runtime};
    CHECK_FALSE(landing.canOpen());
    landing.setPollIdDraft("   ");
    CHECK_FALSE(landing.canOpen());
    landing.setPollIdDraft("  abc123  ");
    CHECK(landing.canOpen());
    CHECK(landing.trimmedPollId() == "abc123");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_polls_tests`
Expected: FAIL — `'controllers/create_poll_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Replace `examples/polls/CMakeLists.txt` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# polls — rung 3 of the application ladder (examples/polls/README.md).
# morph_add_rung() wires the standard targets (cmake/morph_add_rung.cmake); this
# file adds the sources its directory conventions do not cover, then the
# browser build's server url.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME polls)

# The authorizer (src/auth/) sits outside the src/models, src/db and src/app globs.
if(TARGET ladder_polls_lib)
    target_sources(ladder_polls_lib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/polls_authorizer.cpp")
endif()

# The browser build has no ladder_polls_lib (no ODBC in a browser). The authorizer has no persistence
# dependency, so a client that names it compiles it into the client library instead.
if(TARGET ladder_polls_app AND NOT TARGET ladder_polls_lib)
    target_sources(ladder_polls_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/polls_authorizer.cpp")
endif()

# ── The browser build's server url ──────────────────────────────────────────
# A page served from a static bundle has no argv to read --server from. Port 8767 is
# ladder_polls_server's own default.
if(EMSCRIPTEN AND TARGET polls)
    if(NOT DEFINED MORPH_LADDER_POLLS_WASM_SERVER_URL)
        set(MORPH_LADDER_POLLS_WASM_SERVER_URL "ws://127.0.0.1:8767" CACHE STRING
            "URL polls' browser build connects to; must be a reachable ladder_polls_server.")
    endif()
    target_compile_definitions(polls PRIVATE
        MORPH_LADDER_POLLS_WASM_SERVER_URL="${MORPH_LADDER_POLLS_WASM_SERVER_URL}"
    )
endif()
```

Create `examples/polls/app/controllers/poll_text.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <string>
#include <string_view>

#include "polls/core/types.hpp"
#include "polls/units.hpp"

/// @file
/// The text every polls controller shows, in one place.

namespace polls::client {

/// @brief A screen's status line.
struct StatusLine {
    std::string text;      ///< Empty when there is nothing to report.
    bool isError = false;  ///< Whether `text` reports a failure.

    /// @brief Field-wise equality.
    bool operator==(StatusLine const&) const = default;
};

/// @brief @p text without leading and trailing blanks.
/// @param text The text.
/// @return The trimmed copy.
[[nodiscard]] std::string trimmed(std::string_view text);

/// @brief A count as text (`morph::units::toString`).
/// @param count The count.
/// @return Its text.
[[nodiscard]] std::string countText(Count const& count);

/// @brief A choice's wire name: "Yes", "IfNeedBe" or "No".
/// @param choice The choice.
/// @return Its name.
[[nodiscard]] std::string choiceName(VoteChoice choice);

/// @brief The choice a wire name stands for; anything unknown is `No`, the safe default for a vote row.
/// @param name A wire name.
/// @return The choice.
[[nodiscard]] VoteChoice choiceNamed(std::string_view name);

/// @brief A choice as a person reads it: "Yes", "If need be" or "No".
/// @param choice The choice.
/// @return Its label.
[[nodiscard]] std::string choiceLabel(VoteChoice choice);

/// @brief An outcome as a status line.
/// @param text What happened.
/// @return @p text, not an error.
[[nodiscard]] StatusLine infoLine(std::string text);

/// @brief A failure as a status line.
/// @param error The failure; must not be null.
/// @return `errorMessage(error)`, marked as an error.
[[nodiscard]] StatusLine failureLine(std::exception_ptr const& error);

}  // namespace polls::client
```

Create `examples/polls/app/controllers/poll_text.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/poll_text.hpp"

#include <morph/reactive/control.hpp>
#include <morph/util/quantity.hpp>
#include <utility>

namespace polls::client {

std::string trimmed(std::string_view text) {
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    auto const last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

std::string countText(Count const& count) { return ::morph::units::toString(count); }

std::string choiceName(VoteChoice choice) {
    switch (choice) {
        case VoteChoice::Yes:
            return "Yes";
        case VoteChoice::IfNeedBe:
            return "IfNeedBe";
        case VoteChoice::No:
            return "No";
        default:
            return "No";
    }
}

VoteChoice choiceNamed(std::string_view name) {
    if (name == "Yes") {
        return VoteChoice::Yes;
    }
    if (name == "IfNeedBe") {
        return VoteChoice::IfNeedBe;
    }
    return VoteChoice::No;
}

std::string choiceLabel(VoteChoice choice) { return choice == VoteChoice::IfNeedBe ? "If need be" : choiceName(choice); }

StatusLine infoLine(std::string text) { return StatusLine{.text = std::move(text), .isError = false}; }

StatusLine failureLine(std::exception_ptr const& error) {
    return StatusLine{.text = ::morph::reactive::errorMessage(error), .isError = true};
}

}  // namespace polls::client
```

Create `examples/polls/app/controllers/landing_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <string>

/// @file
/// The landing screen: the poll id a participant pastes.

namespace polls::client {

/// @brief Whether this build may create polls. Creating is the organizer's, from a native client; a
///        participant in a browser follows a shared link (the README's design decision 6).
enum class CreateAccess : std::uint8_t {
    Offered,  ///< The landing screen offers "Create a new poll".
    Hidden,   ///< It does not, and the create screen is unreachable.
};

/// @brief The landing screen's controller: the pasted poll id.
class LandingController {
public:
    /// @param runtime The runtime.
    explicit LandingController(::morph::reactive::Runtime& runtime);

    ~LandingController() = default;
    LandingController(LandingController const&) = delete;
    LandingController& operator=(LandingController const&) = delete;
    LandingController(LandingController&&) = delete;
    LandingController& operator=(LandingController&&) = delete;

    /// @brief What has been typed. Tracked.
    /// @return The draft.
    [[nodiscard]] std::string const& pollIdDraft() const { return _draft.get(); }

    /// @brief Replaces the draft.
    /// @param text The typed text.
    void setPollIdDraft(std::string text) { _draft.set(std::move(text)); }

    /// @brief Whether "Open" acts. Tracked.
    /// @return True when the trimmed draft is non-empty.
    [[nodiscard]] bool canOpen() const { return _canOpen.get(); }

    /// @brief The draft, trimmed: the id "Open" opens. Tracked.
    /// @return The id.
    [[nodiscard]] std::string const& trimmedPollId() const { return _trimmed.get(); }

private:
    ::morph::reactive::Signal<std::string> _draft;
    ::morph::reactive::Computed<std::string> _trimmed;
    ::morph::reactive::Computed<bool> _canOpen;
};

}  // namespace polls::client
```

Create `examples/polls/app/controllers/landing_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/landing_controller.hpp"

#include "controllers/poll_text.hpp"

namespace polls::client {

LandingController::LandingController(::morph::reactive::Runtime& runtime)
    : _draft{runtime, std::string{}},
      _trimmed{runtime, [this] { return trimmed(_draft.get()); }},
      _canOpen{runtime, [this] { return !_trimmed.get().empty(); }} {}

}  // namespace polls::client
```

Create `examples/polls/app/controllers/create_poll_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <vector>

#include "controllers/poll_text.hpp"
#include "polls/models/poll_model.hpp"

/// @file
/// The organizer's create-poll screen.

namespace polls::client {

/// @brief One candidate option as it is being typed.
struct OptionDraft {
    std::int64_t key = 0;  ///< Stable for the draft's life and never reused; the row's key.
    std::string label;     ///< What has been typed.

    /// @brief Field-wise equality.
    bool operator==(OptionDraft const&) const = default;
};

/// @brief The create-poll screen's controller.
///
/// `CreatePoll::options` is a list of objects, which a schema form does not edit, so the option list is a
/// signal of drafts the view renders with a keyed `forEach`: between `kMinOptions` and `kMaxOptions`
/// drafts, each with a key that survives every add and remove, so a removal leaves the other rows' widgets
/// (and what is typed into them) alone. `CreatePoll` is dispatched through a plain handler — it carries no
/// key, so it never attaches to a shared instance.
class CreatePollController {
public:
    /// @param runtime The runtime; its owner must be @p callbacks.
    /// @param bridge The client's bridge.
    /// @param callbacks Where replies are delivered.
    CreatePollController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                         ::morph::exec::IExecutor& callbacks);

    ~CreatePollController() = default;
    CreatePollController(CreatePollController const&) = delete;
    CreatePollController& operator=(CreatePollController const&) = delete;
    CreatePollController(CreatePollController&&) = delete;
    CreatePollController& operator=(CreatePollController&&) = delete;

    /// @brief The typed title. Tracked.
    /// @return It.
    [[nodiscard]] std::string const& title() const { return _title.get(); }

    /// @brief Replaces the title.
    /// @param text The typed text.
    void setTitle(std::string text) { _title.set(std::move(text)); }

    /// @brief The option drafts — the signal the view's `forEach` renders.
    /// @return The signal.
    [[nodiscard]] ::morph::reactive::Signal<std::vector<OptionDraft>> const& options() const noexcept {
        return _options;
    }

    /// @brief Appends an empty draft; refused at `kMaxOptions`.
    void addOption();

    /// @brief Removes a draft; refused at `kMinOptions`.
    /// @param key The draft.
    void removeOption(std::int64_t key);

    /// @brief Replaces a draft's label.
    /// @param key The draft.
    /// @param label The typed text.
    void setOptionLabel(std::int64_t key, std::string label);

    /// @brief Whether "+ add option" acts. Tracked.
    /// @return True below `kMaxOptions`.
    [[nodiscard]] bool canAdd() const { return _canAdd.get(); }

    /// @brief Whether a row's "remove" acts. Tracked.
    /// @return True above `kMinOptions`.
    [[nodiscard]] bool canRemove() const { return _canRemove.get(); }

    /// @brief Whether "Create poll" acts. Tracked.
    /// @return True with a non-blank title, every label non-blank, and nothing in flight.
    [[nodiscard]] bool canSubmit() const { return _canSubmit.get(); }

    /// @brief Creates the poll; does nothing unless `canSubmit()`.
    void submit();

    /// @brief Whether a poll was created and its ids are showing. Tracked.
    /// @return True after a successful create, until `startOver()`.
    [[nodiscard]] bool created() const { return _result.get().has_value(); }

    /// @brief Whether the editor shows (no poll created yet). Tracked.
    /// @return `!created()`.
    [[nodiscard]] bool editing() const { return !created(); }

    /// @brief The created poll's shareable id. Tracked.
    /// @return It, or empty.
    [[nodiscard]] std::string const& pollId() const { return _pollId.get(); }

    /// @brief The created poll's admin token, shown once. Tracked.
    /// @return It, or empty.
    [[nodiscard]] std::string const& adminToken() const { return _adminToken.get(); }

    /// @brief The created poll's participant token. Tracked.
    /// @return It, or empty.
    [[nodiscard]] std::string const& participantToken() const { return _participantToken.get(); }

    /// @brief Back to an empty editor.
    void startOver();

    /// @brief The status line's text. Tracked.
    /// @return The text, or empty.
    [[nodiscard]] std::string const& statusText() const { return _status.get().text; }

    /// @brief Whether the status line reports a failure. Tracked.
    /// @return True for a failure.
    [[nodiscard]] bool statusIsError() const { return _status.get().isError; }

    /// @brief Whether there is a status to show. Tracked.
    /// @return True when the text is non-empty.
    [[nodiscard]] bool hasStatus() const { return !_status.get().text.empty(); }

private:
    [[nodiscard]] std::vector<OptionDraft> freshDrafts();

    ::morph::reactive::Runtime* _rt;
    ::morph::bridge::BridgeHandler<PollModel> _creator;
    std::int64_t _nextKey = 1;
    ::morph::reactive::Signal<std::string> _title;
    ::morph::reactive::Signal<std::vector<OptionDraft>> _options;
    ::morph::reactive::Signal<StatusLine> _status;
    ::morph::reactive::Signal<std::optional<CreatePollResult>> _result;
    ::morph::reactive::Mutation<CreatePoll> _create;
    ::morph::reactive::Computed<bool> _canAdd;
    ::morph::reactive::Computed<bool> _canRemove;
    ::morph::reactive::Computed<bool> _canSubmit;
    ::morph::reactive::Computed<std::string> _pollId;
    ::morph::reactive::Computed<std::string> _adminToken;
    ::morph::reactive::Computed<std::string> _participantToken;
    ::morph::reactive::Effect _onCreated;
    ::morph::reactive::Effect _onFailed;
};

}  // namespace polls::client
```

Create `examples/polls/app/controllers/create_poll_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/create_poll_controller.hpp"

#include <algorithm>
#include <utility>

namespace polls::client {

CreatePollController::CreatePollController(::morph::reactive::Runtime& runtime, ::morph::bridge::Bridge& bridge,
                                           ::morph::exec::IExecutor& callbacks)
    : _rt{&runtime},
      _creator{bridge, &callbacks},
      _title{runtime, std::string{}},
      _options{runtime, freshDrafts()},
      _status{runtime, StatusLine{}},
      _result{runtime, std::nullopt},
      _create{runtime, _creator},
      _canAdd{runtime, [this] { return _options.get().size() < kMaxOptions; }},
      _canRemove{runtime, [this] { return _options.get().size() > kMinOptions; }},
      _canSubmit{runtime,
                 [this] {
                     auto const& drafts = _options.get();
                     return !trimmed(_title.get()).empty() && drafts.size() >= kMinOptions &&
                            drafts.size() <= kMaxOptions &&
                            std::ranges::all_of(drafts,
                                                [](OptionDraft const& draft) { return !trimmed(draft.label).empty(); }) &&
                            !_create.pending();
                 }},
      _pollId{runtime,
              [this] {
                  auto const& result = _result.get();
                  return result.has_value() ? result->pollId : std::string{};
              }},
      _adminToken{runtime,
                  [this] {
                      auto const& result = _result.get();
                      return result.has_value() && result->adminToken.hasValue() ? *result->adminToken : std::string{};
                  }},
      _participantToken{runtime,
                        [this] {
                            auto const& result = _result.get();
                            return result.has_value() && result->participantToken.hasValue() ? *result->participantToken
                                                                                             : std::string{};
                        }},
      _onCreated{runtime,
                 [this] {
                     auto const& created = _create.lastResult();
                     if (!created.has_value()) {
                         return;
                     }
                     _rt->untracked([&] {
                         _result.set(*created);
                         _status.set(infoLine("poll created: copy the admin token before leaving this screen"));
                     });
                 }},
      _onFailed{runtime,
                [this] {
                    if (auto const error = _create.error()) {
                        _status.set(failureLine(error));
                    }
                }} {}

std::vector<OptionDraft> CreatePollController::freshDrafts() {
    std::vector<OptionDraft> drafts;
    for (std::size_t i = 0; i < kMinOptions; ++i) {
        drafts.push_back(OptionDraft{.key = _nextKey++, .label = {}});
    }
    return drafts;
}

void CreatePollController::addOption() {
    if (_options.peek().size() >= kMaxOptions) {
        return;
    }
    _options.mutate([this](std::vector<OptionDraft>& drafts) {
        drafts.push_back(OptionDraft{.key = _nextKey++, .label = {}});
    });
}

void CreatePollController::removeOption(std::int64_t key) {
    if (_options.peek().size() <= kMinOptions) {
        return;
    }
    _options.mutate([key](std::vector<OptionDraft>& drafts) {
        std::erase_if(drafts, [key](OptionDraft const& draft) { return draft.key == key; });
    });
}

void CreatePollController::setOptionLabel(std::int64_t key, std::string label) {
    _options.mutate([key, &label](std::vector<OptionDraft>& drafts) {
        for (auto& draft : drafts) {
            if (draft.key == key) {
                draft.label = std::move(label);
                return;
            }
        }
    });
}

void CreatePollController::submit() {
    if (!_canSubmit.get()) {
        return;
    }
    CreatePoll action{.title = _title.peek(), .options = {}};
    for (auto const& draft : _options.peek()) {
        action.options.push_back(CreatePollOption{.label = draft.label});
    }
    _create.run(std::move(action));
}

void CreatePollController::startOver() {
    _rt->batch([this] {
        _result.set(std::nullopt);
        _title.set(std::string{});
        _options.set(freshDrafts());
        _status.set(StatusLine{});
    });
}

}  // namespace polls::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_polls_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/polls/ladder_polls_tests "[polls][controller]"
```

Expected: PASS. Mutation check: in `removeOption`, replace the `erase_if` with rebuilding the list under fresh keys
(`drafts = …` with `_nextKey++` per remaining draft). Expected FAIL in "option drafts: the bounds hold and a
removal keeps the other drafts' keys" (`keysOf(create) == expected`). Restore. Second: delete the
`size() <= kMinOptions` guard. Expected FAIL in the same case (`keysOf(create) == initial`). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/polls/CMakeLists.txt examples/polls/app examples/polls/tests/client_fixture.hpp \
    examples/polls/tests/test_create_poll_controller.cpp
git commit -m "wip(polls): the landing and create-poll controllers, option drafts as a signal

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 19: The vote controller — open when connected, state, votes, admin token

A keyed attach (`OpenPoll`) is the one call a remote transport refuses outright while its socket is still
connecting (`QtWebSocketBackend::bindModel`, "disconnected"): it may re-point a live instance, so it is never
queued. The vote controller therefore issues `OpenPoll` only while the connection is ready
(`examples::Connection::ready()`, handed in as a tracked predicate): an Effect over "the poll the user asked for"
and that readiness, so a browser following a `?poll=` link opens it the moment its socket connects.

**Files:**
- Create: `examples/polls/app/controllers/vote_controller.{hpp,cpp}`
- Modify: `examples/polls/tests/client_fixture.hpp` — `VoteFixture`, `openAndWait()`
- Test: `examples/polls/tests/test_vote_controller.cpp`

**Interfaces:**
- Consumes: `Query`, `Mutation`, `Computed`, `Effect`, `Signal::mutate`, `Runtime::{batch, untracked,
  isFlushRequested}`; `bridge::BridgeHandler<PollModel, bridge::AllowShared>`; `Bridge::setDefaultSession`,
  `session::Context`; `polls::{OpenPoll, GetPollState, GetPollStateResult, SubmitVotes, UpdateVotes, OneVote,
  OptionId, Finalized}`; `poll_text` (Task 18); `examples::Wiring{runtime, scheduler, bridge, callbacks}`
  (`examples/common/app/wiring.hpp`, Part 6 — the controller needs all four, the scheduler from Task 21 on);
  `reactive::testing::ManualScheduler` (the fixture's).
- Produces: `polls::client::{OptionRow, CommentRow, VoteController}`; `VoteController(examples::Wiring,
  std::function<bool()> connected)` with
  `open()`, `close()`, `refresh()`, `attached()`, `opening()`, `title()`, `finalized()`, `participantName()`,
  `setParticipantName()`, `options()`, `pickFor()`, `setPick()`, `canChangeVotes()`, `canVote()`, `voteLabel()`,
  `vote()`, `comments()`, `commentsHeading()`, `adminTokenDraft()`, `setAdminTokenDraft()`, `canUseAdminToken()`,
  `useAdminToken()`, `statusText()`, `statusIsError()`, `hasStatus()`; `polls::testing::{VoteFixture,
  openAndWait}`.

- [ ] **Step 1: Write the failing tests**

Append to `examples/polls/tests/client_fixture.hpp`, inside `namespace polls::testing` (add
`#include <morph/reactive/runtime.hpp>`, `#include <morph/reactive/signal.hpp>`,
`#include <morph/reactive/testing/manual_scheduler.hpp>`, `#include "app/wiring.hpp"`,
`#include "controllers/vote_controller.hpp"`):

```cpp
/// @brief One client of one rig with a vote controller. Time advances only through `scheduler`; whether the
///        transport is connected is `connected`, true unless a test says otherwise.
struct VoteFixture {
    /// @param mode The deployment shape.
    explicit VoteFixture(::morph::ladder::testkit::Mode mode)
        : rig{pollsRig(mode)},
          runtime{*rig->executor()},
          connected{runtime, true},
          vote{::morph::examples::Wiring{.runtime = runtime,
                                         .scheduler = scheduler,
                                         .bridge = rig->bridge(0),
                                         .callbacks = *rig->executor()},
               [this] { return connected.get(); }} {}

    std::unique_ptr<::morph::ladder::testkit::BackendRig> rig;  ///< Backend and executors.
    ::morph::reactive::Runtime runtime;                         ///< Owned by the rig's client executor.
    ::morph::reactive::testing::ManualScheduler scheduler;      ///< The vote screen's timers.
    ::morph::reactive::Signal<bool> connected;                  ///< Stands in for `Connection::ready()`.
    client::VoteController vote;                                ///< The controller under test.
};

/// @brief Opens @p pollId and waits until its state shows.
/// @param vote The controller.
/// @param pollId The poll.
inline void openAndWait(client::VoteController& vote, std::string const& pollId) {
    vote.open(pollId);
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return vote.attached() && !vote.options().empty(); }));
}
```

Create `examples/polls/tests/test_vote_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <string>

#include "client_fixture.hpp"
#include "controllers/create_poll_controller.hpp"
#include "controllers/vote_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using polls::testing::VoteFixture;

}  // namespace

TEST_CASE("The vote screen opens a poll the create screen made, all three backend modes", "[polls][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    VoteFixture client{mode};
    polls::client::CreatePollController create{client.runtime, client.rig->bridge(0), *client.rig->executor()};
    auto const drafts = create.options().peek();
    create.setTitle("Team offsite");
    create.setOptionLabel(drafts[0].key, "2026-09-01");
    create.setOptionLabel(drafts[1].key, "2026-09-02");
    create.submit();
    REQUIRE(pumpUntil([&] { return create.created(); }));

    polls::testing::openAndWait(client.vote, create.pollId());
    CHECK(client.vote.title() == "Team offsite");
    CHECK_FALSE(client.vote.finalized());
    REQUIRE(client.vote.options().size() == 2);
    auto const& first = client.vote.options()[0];
    CHECK(first.label == "2026-09-01  (#" + std::to_string(first.optionId) + ")");
    CHECK(first.tally == "yes 0 · if need be 0 · no 0");
}

TEST_CASE("refresh re-reads the same poll's state, all three backend modes", "[polls][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    VoteFixture client{mode};
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    polls::testing::openAndWait(client.vote, poll.pollId);
    client.vote.refresh();
    REQUIRE(pumpUntil([&] { return client.vote.title() == "T"; }));
    CHECK(client.vote.options().size() == 2);
}

TEST_CASE("A vote is tallied, and a second one replaces it, all three backend modes", "[polls][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    VoteFixture client{mode};
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    polls::testing::openAndWait(client.vote, poll.pollId);
    std::int64_t const optionId = client.vote.options()[0].optionId;

    CHECK_FALSE(client.vote.canVote());  // no name yet
    client.vote.vote();
    CHECK(client.vote.voteLabel() == "Submit my votes");

    client.vote.setParticipantName("alice");
    CHECK(client.vote.pickFor(optionId) == polls::VoteChoice::No);
    client.vote.setPick(optionId, polls::VoteChoice::Yes);
    REQUIRE(client.vote.canVote());
    client.vote.vote();
    REQUIRE(pumpUntil([&] { return client.vote.options()[0].tally == "yes 1 · if need be 0 · no 0"; }));
    CHECK(client.vote.voteLabel() == "Update my votes");

    client.vote.setPick(optionId, polls::VoteChoice::No);
    client.vote.vote();
    REQUIRE(pumpUntil([&] { return client.vote.options()[0].tally == "yes 0 · if need be 0 · no 1"; }));
}

TEST_CASE("Opening an unknown poll reports the model's message and attaches nothing", "[polls][controller]") {
    DbFixture fixture;
    VoteFixture client{Mode::Local};
    client.vote.open("no-such-poll");
    REQUIRE(pumpUntil([&] { return client.vote.statusIsError(); }));
    CHECK_FALSE(client.vote.attached());
    CHECK(client.vote.options().empty());
}

TEST_CASE("OpenPoll waits for the connection, and goes out the moment it is ready", "[polls][controller]") {
    DbFixture fixture;
    VoteFixture client{Mode::Local};
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    client.connected.set(false);
    client.vote.open(poll.pollId);
    REQUIRE(pumpUntil([&] { return !client.runtime.isFlushRequested(); }));
    CHECK_FALSE(client.vote.opening());  // a keyed attach on a connecting socket would be refused, not queued
    CHECK_FALSE(client.vote.attached());

    client.connected.set(true);
    REQUIRE(pumpUntil([&] { return client.vote.attached(); }));
    CHECK(client.vote.title() == "T");
}

TEST_CASE("a reply for a poll the user has left never attaches it", "[polls][controller]") {
    DbFixture fixture;
    VoteFixture client{Mode::Local};
    auto const poll = polls::testing::seedPoll(*client.rig, "Left behind", {"1", "2"});
    // Leaves in the very flush that issues the OpenPoll, before any reply can arrive.
    bool issued = false;
    morph::reactive::Effect const leaveAtOnce{client.runtime, [&] {
                                                  if (client.vote.opening() && !issued) {
                                                      issued = true;
                                                      client.runtime.untracked([&] { client.vote.close(); });
                                                  }
                                              }};
    client.vote.open(poll.pollId);
    // The OpenPoll went out, its reply has landed, and every flush it caused has run.
    REQUIRE(pumpUntil([&] { return issued && !client.vote.opening() && !client.runtime.isFlushRequested(); }));
    CHECK_FALSE(client.vote.attached());
    CHECK(client.vote.title().empty());
    CHECK(client.vote.options().empty());
}

TEST_CASE("Before a poll is open nothing is issued, and the admin token is installed on demand",
          "[polls][controller]") {
    DbFixture fixture;
    VoteFixture client{Mode::Local};
    CHECK_FALSE(client.vote.attached());
    CHECK_FALSE(client.vote.opening());
    CHECK(client.vote.options().empty());
    CHECK_FALSE(client.vote.canVote());
    CHECK_FALSE(client.vote.canUseAdminToken());
    client.vote.setAdminTokenDraft("secret");
    REQUIRE(client.vote.canUseAdminToken());
    client.vote.useAdminToken();
    CHECK(client.vote.statusText() == "admin token in use");
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/all --target ladder_polls_tests`
Expected: FAIL — `'controllers/vote_controller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/polls/app/controllers/vote_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <vector>

#include "app/wiring.hpp"
#include "controllers/poll_text.hpp"
#include "polls/models/poll_model.hpp"

/// @file
/// The vote screen: one open poll, its tallies, the participant's votes, comments, the admin's finalize,
/// and the live activity feed.

namespace polls::client {

/// @brief One option, with its tally, as the vote grid shows it.
struct OptionRow {
    std::int64_t optionId = 0;  ///< The option; also the row's key.
    std::string label;          ///< "2026-09-01  (#12)": the id is what the finalize form asks for.
    std::string tally;          ///< "yes 1 · if need be 0 · no 2".

    /// @brief Field-wise equality.
    bool operator==(OptionRow const&) const = default;
};

/// @brief One comment.
struct CommentRow {
    std::int64_t position = 0;  ///< Its place in the poll's comment list; comments are append-only.
    std::string line;           ///< "alice: works for me".

    /// @brief Field-wise equality.
    bool operator==(CommentRow const&) const = default;
};

/// @brief The vote screen's controller.
///
/// Every action on an open poll goes through **one** `BridgeHandler<PollModel, AllowShared>`: `OpenPoll`
/// attaches it to the poll's shared instance, and every later action must run on that same handler or it
/// has no instance to run against. `open()` only records which poll the user asked for; an Effect issues
/// `OpenPoll` once that request exists and the connection is ready, because a remote transport refuses a
/// keyed attach while it connects. A reply is accepted only for the poll the user is still on. The poll's
/// state is a `Query<GetPollState>` keyed on the attachment, which votes and form submissions invalidate.
class VoteController {
public:
    /// @param wiring The runtime (its owner must be the callbacks executor), the scheduler the activity feed
    ///               runs on, the bridge (the admin token is installed on it) and the executor replies are
    ///               delivered on. All borrowed: they must outlive this controller.
    /// @param connected Tracked: whether the transport can carry a keyed attach now (`Connection::ready()`).
    VoteController(::morph::examples::Wiring wiring, std::function<bool()> connected);

    ~VoteController() = default;
    VoteController(VoteController const&) = delete;
    VoteController& operator=(VoteController const&) = delete;
    VoteController(VoteController&&) = delete;
    VoteController& operator=(VoteController&&) = delete;

    /// @brief Asks for a poll: `OpenPoll` goes out as soon as the connection is ready.
    /// @param pollId The poll's shareable id.
    void open(std::string pollId);

    /// @brief Leaves the poll: its state goes idle, and a reply still on its way is ignored.
    void close();

    /// @brief Re-reads the open poll's state.
    void refresh() { _state.refetch(); }

    /// @brief Whether a poll is open. Tracked.
    /// @return True once `OpenPoll` succeeded for the poll the user is on.
    [[nodiscard]] bool attached() const { return _attached.get().has_value(); }

    /// @brief Whether an `OpenPoll` is in flight. Tracked.
    /// @return True from issue to reply.
    [[nodiscard]] bool opening() const { return _open.pending(); }

    /// @brief The poll's title, "  (finalized)" appended once finalized; "opening…" until it loads. Tracked.
    /// @return The title, or empty while no poll is asked for.
    [[nodiscard]] std::string const& title() const { return _title.get(); }

    /// @brief Whether the open poll is finalized. Tracked.
    /// @return True once finalized.
    [[nodiscard]] bool finalized() const { return _finalized.get(); }

    /// @brief The participant's typed name. Tracked.
    /// @return It.
    [[nodiscard]] std::string const& participantName() const { return _participant.get(); }

    /// @brief Replaces the participant's name.
    /// @param name The typed name.
    void setParticipantName(std::string name) { _participant.set(std::move(name)); }

    /// @brief The open poll's options with their tallies. Tracked.
    /// @return The rows.
    [[nodiscard]] std::vector<OptionRow> const& options() const { return _options.get(); }

    /// @brief The participant's pick for an option. Tracked.
    /// @param optionId The option.
    /// @return The pick; `No` until one is made.
    [[nodiscard]] VoteChoice pickFor(std::int64_t optionId) const;

    /// @brief Records the participant's pick for an option.
    /// @param optionId The option.
    /// @param choice The pick.
    void setPick(std::int64_t optionId, VoteChoice choice);

    /// @brief Whether the picks may change. Tracked.
    /// @return True while a poll is open and not finalized.
    [[nodiscard]] bool canChangeVotes() const { return _canChangeVotes.get(); }

    /// @brief Whether the vote button acts. Tracked.
    /// @return True with a poll open and not finalized, a non-blank name, and no vote in flight.
    [[nodiscard]] bool canVote() const { return _canVote.get(); }

    /// @brief "Submit my votes" before the first vote, "Update my votes" after. Tracked.
    /// @return The label.
    [[nodiscard]] std::string const& voteLabel() const { return _voteLabel.get(); }

    /// @brief Submits the picks (first time) or replaces them; does nothing unless `canVote()`.
    void vote();

    /// @brief The open poll's comments. Tracked.
    /// @return The rows, oldest first.
    [[nodiscard]] std::vector<CommentRow> const& comments() const { return _comments.get(); }

    /// @brief "Comments (N)". Tracked.
    /// @return The heading.
    [[nodiscard]] std::string const& commentsHeading() const { return _commentsHeading.get(); }

    /// @brief The typed admin token. Tracked.
    /// @return It.
    [[nodiscard]] std::string const& adminTokenDraft() const { return _adminDraft.get(); }

    /// @brief Replaces the typed admin token.
    /// @param token The typed text.
    void setAdminTokenDraft(std::string token) { _adminDraft.set(std::move(token)); }

    /// @brief Whether "use" acts. Tracked.
    /// @return True when a token is typed.
    [[nodiscard]] bool canUseAdminToken() const { return _canUseAdminToken.get(); }

    /// @brief Installs the typed token as the bridge's default session — the poll's whole admin identity.
    void useAdminToken();

    /// @brief The status line's text. Tracked.
    /// @return The text, or empty.
    [[nodiscard]] std::string const& statusText() const { return _status.get().text; }

    /// @brief Whether the status line reports a failure. Tracked.
    /// @return True for a failure.
    [[nodiscard]] bool statusIsError() const { return _status.get().isError; }

    /// @brief Whether there is a status to show. Tracked.
    /// @return True when the text is non-empty.
    [[nodiscard]] bool hasStatus() const { return !_status.get().text.empty(); }

private:
    /// @brief One request to open a poll; the serial makes a repeated request for the same poll a change.
    struct OpenRequest {
        std::string pollId;       ///< The poll.
        std::uint64_t serial = 0;  ///< Distinguishes repeated requests.

        /// @brief Field-wise equality.
        bool operator==(OpenRequest const&) const = default;
    };

    /// @brief The poll the handler is attached to, and where its event log stood then.
    struct Attachment {
        std::string pollId;  ///< The poll.
        PollEventId cursor;  ///< `GetPollStateResult::lastEventId` at the open.

        /// @brief Field-wise equality.
        bool operator==(Attachment const&) const = default;
    };

    void reportError(std::exception_ptr const& error);

    ::morph::reactive::Runtime* _rt;
    ::morph::bridge::Bridge* _bridge;
    std::function<bool()> _connected;
    ::morph::bridge::BridgeHandler<PollModel, ::morph::bridge::AllowShared> _handler;
    std::uint64_t _serial = 0;
    ::morph::reactive::Signal<std::optional<OpenRequest>> _requested;
    ::morph::reactive::Signal<std::optional<Attachment>> _attached;
    ::morph::reactive::Signal<std::string> _participant;
    ::morph::reactive::Signal<std::map<std::int64_t, VoteChoice>> _picks;
    ::morph::reactive::Signal<bool> _hasVoted;
    ::morph::reactive::Signal<std::string> _adminDraft;
    ::morph::reactive::Signal<StatusLine> _status;
    ::morph::reactive::Query<GetPollState> _state;
    ::morph::reactive::Mutation<OpenPoll> _open;
    ::morph::reactive::Mutation<SubmitVotes> _submit;
    ::morph::reactive::Mutation<UpdateVotes> _update;
    ::morph::reactive::Computed<std::string> _title;
    ::morph::reactive::Computed<bool> _finalized;
    ::morph::reactive::Computed<std::vector<OptionRow>> _options;
    ::morph::reactive::Computed<std::vector<CommentRow>> _comments;
    ::morph::reactive::Computed<std::string> _commentsHeading;
    ::morph::reactive::Computed<bool> _canChangeVotes;
    ::morph::reactive::Computed<bool> _canVote;
    ::morph::reactive::Computed<std::string> _voteLabel;
    ::morph::reactive::Computed<bool> _canUseAdminToken;
    ::morph::reactive::Effect _onOpened;
    ::morph::reactive::Effect _onOpenFailed;
    ::morph::reactive::Effect _onStateFailed;
    ::morph::reactive::Effect _onSubmitFailed;
    ::morph::reactive::Effect _onUpdateFailed;
    // Last: it issues OpenPoll, so everything a reply touches exists before the first can go out.
    ::morph::reactive::Effect _issueOpen;
};

}  // namespace polls::client
```

Create `examples/polls/app/controllers/vote_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/vote_controller.hpp"

#include <morph/session/session.hpp>
#include <utility>

namespace polls::client {

namespace {

[[nodiscard]] OptionRow optionRowOf(PollOptionView const& option) {
    std::int64_t const optionId = *option.id;
    return OptionRow{.optionId = optionId,
                     .label = option.label + "  (#" + std::to_string(optionId) + ")",
                     .tally = "yes " + countText(option.yesCount) + " · if need be " + countText(option.ifNeedBeCount) +
                              " · no " + countText(option.noCount)};
}

}  // namespace

VoteController::VoteController(::morph::examples::Wiring wiring, std::function<bool()> connected)
    : _rt{&wiring.runtime},
      _bridge{&wiring.bridge},
      _connected{std::move(connected)},
      _handler{wiring.bridge, &wiring.callbacks},
      _requested{wiring.runtime, std::nullopt},
      _attached{wiring.runtime, std::nullopt},
      _participant{wiring.runtime, std::string{}},
      _picks{wiring.runtime, std::map<std::int64_t, VoteChoice>{}},
      _hasVoted{wiring.runtime, false},
      _adminDraft{wiring.runtime, std::string{}},
      _status{wiring.runtime, StatusLine{}},
      _state{wiring.runtime, _handler,
             [this]() -> std::optional<GetPollState> {
                 return _attached.get().has_value() ? std::optional<GetPollState>{GetPollState{}} : std::nullopt;
             }},
      _open{wiring.runtime, _handler},
      _submit{wiring.runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_state}}},
      _update{wiring.runtime, _handler, ::morph::reactive::MutationOptions{.invalidates = {&_state}}},
      _title{wiring.runtime,
             [this] {
                 auto const& state = _state.value();
                 if (!state.has_value()) {
                     return _requested.get().has_value() ? std::string{"opening…"} : std::string{};
                 }
                 return state->finalized == Finalized::Yes ? state->title + "  (finalized)" : state->title;
             }},
      _finalized{wiring.runtime,
                 [this] {
                     auto const& state = _state.value();
                     return state.has_value() && state->finalized == Finalized::Yes;
                 }},
      _options{wiring.runtime,
               [this] {
                   std::vector<OptionRow> rows;
                   if (auto const& state = _state.value()) {
                       for (auto const& option : state->options) {
                           rows.push_back(optionRowOf(option));
                       }
                   }
                   return rows;
               }},
      _comments{wiring.runtime,
                [this] {
                    std::vector<CommentRow> rows;
                    if (auto const& state = _state.value()) {
                        std::int64_t position = 0;
                        for (auto const& comment : state->comments) {
                            rows.push_back(CommentRow{.position = position++,
                                                      .line = comment.participantName + ": " + comment.body});
                        }
                    }
                    return rows;
                }},
      _commentsHeading{wiring.runtime, [this] { return "Comments (" + std::to_string(_comments.get().size()) + ")"; }},
      _canChangeVotes{wiring.runtime,
                      [this] { return _attached.get().has_value() && _state.value().has_value() && !_finalized.get(); }},
      _canVote{wiring.runtime,
               [this] {
                   return _canChangeVotes.get() && !trimmed(_participant.get()).empty() && !_submit.pending() &&
                          !_update.pending();
               }},
      _voteLabel{wiring.runtime,
                 [this] { return _hasVoted.get() ? std::string{"Update my votes"} : std::string{"Submit my votes"}; }},
      _canUseAdminToken{wiring.runtime, [this] { return !_adminDraft.get().empty(); }},
      _onOpened{wiring.runtime,
                [this] {
                    auto const& opened = _open.lastResult();
                    if (!opened.has_value()) {
                        return;
                    }
                    _rt->untracked([&] {
                        auto const& requested = _requested.peek();
                        if (!requested.has_value() || requested->pollId != opened->pollId) {
                            return;  // a reply for a poll the user has left
                        }
                        _rt->batch([&] {
                            _picks.set({});
                            _hasVoted.set(false);
                            _status.set(StatusLine{});
                            _attached.set(Attachment{.pollId = opened->pollId, .cursor = opened->lastEventId});
                        });
                    });
                }},
      _onOpenFailed{wiring.runtime,
                    [this] {
                        auto const error = _open.error();
                        if (error != nullptr && _rt->untracked([this] { return _requested.peek().has_value(); })) {
                            _status.set(failureLine(error));
                        }
                    }},
      _onStateFailed{wiring.runtime, [this] { reportError(_state.error()); }},
      _onSubmitFailed{wiring.runtime, [this] { reportError(_submit.error()); }},
      _onUpdateFailed{wiring.runtime, [this] { reportError(_update.error()); }},
      _issueOpen{wiring.runtime, [this] {
                     auto const& requested = _requested.get();
                     if (!requested.has_value() || !_connected()) {
                         return;
                     }
                     OpenPoll action{.pollId = requested->pollId};
                     _rt->untracked([&] { _open.run(std::move(action)); });
                 }} {}

void VoteController::open(std::string pollId) {
    std::string const opening = "opening " + pollId;
    _rt->batch([&] {
        _attached.set(std::nullopt);
        _status.set(infoLine(opening));
        _requested.set(OpenRequest{.pollId = std::move(pollId), .serial = ++_serial});
    });
}

void VoteController::close() {
    _rt->batch([this] {
        _requested.set(std::nullopt);
        _attached.set(std::nullopt);
        _status.set(StatusLine{});
    });
}

VoteChoice VoteController::pickFor(std::int64_t optionId) const {
    auto const& picks = _picks.get();
    auto const found = picks.find(optionId);
    return found == picks.end() ? VoteChoice::No : found->second;
}

void VoteController::setPick(std::int64_t optionId, VoteChoice choice) {
    _picks.mutate([optionId, choice](std::map<std::int64_t, VoteChoice>& picks) { picks[optionId] = choice; });
}

void VoteController::vote() {
    if (!_canVote.get()) {
        return;
    }
    std::vector<OneVote> votes;
    auto const& picks = _picks.peek();
    for (auto const& option : _state.value()->options) {
        auto const found = picks.find(*option.id);
        votes.push_back(OneVote{.optionId = option.id, .choice = found == picks.end() ? VoteChoice::No : found->second});
    }
    std::string const name = _participant.peek();
    if (_hasVoted.peek()) {
        _update.run(UpdateVotes{.participantName = name, .votes = std::move(votes)});
    } else {
        _submit.run(SubmitVotes{.participantName = name, .votes = std::move(votes)});
    }
    _hasVoted.set(true);
}

void VoteController::useAdminToken() {
    ::morph::session::Context session;
    session.token = _adminDraft.peek();
    _bridge->setDefaultSession(session);
    _status.set(infoLine("admin token in use"));
}

void VoteController::reportError(std::exception_ptr const& error) {
    if (error != nullptr) {
        _status.set(failureLine(error));
    }
}

}  // namespace polls::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_polls_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/polls/ladder_polls_tests "[polls][controller]"
```

Expected: PASS. Mutation check: in `_onOpened`, delete the `if (!requested.has_value() || requested->pollId !=
opened->pollId) { return; }` guard. Expected FAIL in "a reply for a poll the user has left never attaches it"
(`CHECK_FALSE(client.vote.attached())`). Restore. Second: in `_issueOpen`, delete `|| !_connected()`. Expected
FAIL in "OpenPoll waits for the connection, and goes out the moment it is ready"
(`CHECK_FALSE(client.vote.opening())`). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/polls/app/controllers examples/polls/tests/client_fixture.hpp examples/polls/tests/test_vote_controller.cpp
git commit -m "wip(polls): the vote controller — open when connected, state, votes, admin token

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 20: The vote screen's three forms, typed, on the attached handler

`AddComment`, `FinalizePoll` and `UndoLastVoteChange` must run on the **same** `AllowShared` handler `OpenPoll`
attached — a second handler would have no instance. A typed `forms::Form<A, PollModel, AllowShared>` over that
handler does exactly that, and `forms::handlerChoiceFetcher(callbacks, _handler)` sends any Choice lookup there
too; `forms::bridgeSubmitter`/`bridgeChoiceFetcher` would not (they make handlers of their own, attached to
nothing). A runtime form would take `forms::handlerSubmitter(callbacks, _handler)` for the same reason.

**Files:**
- Modify: `examples/polls/app/controllers/vote_controller.hpp` — includes, the three form aliases and accessors,
  `canFinalize()`, `onFormSucceeded()`, three form members after `_update`, three `FormSuccess` members before
  `_issueOpen`
- Modify: `examples/polls/app/controllers/vote_controller.cpp` — six constructor initialisers, `onFormSucceeded()`
- Test: `examples/polls/tests/test_vote_forms.cpp`

**Interfaces:**
- Consumes: `forms::Form<A, M, S>` with `S = bridge::AllowShared`, `session()`, `set<Member>()`, `ready()`
  (`typed_form.hpp`); `forms::handlerChoiceFetcher(exec::IExecutor&, Handlers&...)` (`handler_submitter.hpp`, Part 5
  Task 10b); `FormSession::model()`, `FormModel::actionType()`, `submitMode()`, `reset()`; `examples::FormSuccess`.
- Produces: `VoteController::{CommentForm, FinalizeForm, UndoForm, commentForm(), finalizeForm(), undoForm(),
  canFinalize()}`.

- [ ] **Step 1: Write the failing test**

Create `examples/polls/tests/test_vote_forms.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <morph/forms/engine/field_model.hpp>
#include <string>

#include "client_fixture.hpp"
#include "controllers/vote_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using polls::testing::VoteFixture;

void voteFor(polls::client::VoteController& vote, std::int64_t optionId, polls::VoteChoice choice,
             std::string const& expectedTally) {
    vote.setPick(optionId, choice);
    REQUIRE(vote.canVote());
    vote.vote();
    REQUIRE(pumpUntil([&] { return vote.options()[0].tally == expectedTally; }));
}

}  // namespace

TEST_CASE("The vote screen's three forms are AddComment, FinalizePoll and UndoLastVoteChange, each explicit",
          "[polls][controller]") {
    DbFixture fixture;
    VoteFixture client{Mode::Local};
    CHECK(client.vote.commentForm().session().model().actionType() == "AddComment");
    CHECK(client.vote.finalizeForm().session().model().actionType() == "FinalizePoll");
    CHECK(client.vote.undoForm().session().model().actionType() == "UndoLastVoteChange");
    for (auto* session : {&client.vote.commentForm().session(), &client.vote.finalizeForm().session(),
                          &client.vote.undoForm().session()}) {
        CHECK(session->model().submitMode() == morph::forms::SubmitMode::Explicit);
    }
    // What validate() refuses, the typed forms refuse to call ready.
    client.vote.commentForm().set<&polls::AddComment::participantName>(std::string{"alice"});
    CHECK_FALSE(client.vote.commentForm().ready());  // an empty body
    CHECK_FALSE(client.vote.finalizeForm().ready());  // no option id
}

TEST_CASE("A comment through its form shows in the comments, all three backend modes", "[polls][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    VoteFixture client{mode};
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    polls::testing::openAndWait(client.vote, poll.pollId);
    CHECK(client.vote.commentsHeading() == "Comments (0)");

    auto& form = client.vote.commentForm();
    form.set<&polls::AddComment::participantName>(std::string{"alice"});
    form.set<&polls::AddComment::body>(std::string{"works for me"});
    REQUIRE(form.ready());
    form.session().submit();
    REQUIRE(pumpUntil([&] { return client.vote.comments().size() == 1; }));
    CHECK(client.vote.comments().front().line == "alice: works for me");
    CHECK(client.vote.commentsHeading() == "Comments (1)");
    CHECK(client.vote.statusText() == "comment added");
    CHECK_FALSE(form.ready());  // reset after the success
}

TEST_CASE("FinalizePoll is refused without the admin token and finalizes with it, all three backend modes",
          "[polls][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    VoteFixture client{mode};
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    polls::testing::openAndWait(client.vote, poll.pollId);
    std::int64_t const optionId = client.vote.options()[0].optionId;
    REQUIRE(client.vote.canFinalize());

    auto& form = client.vote.finalizeForm();
    form.set<&polls::FinalizePoll::optionId>(polls::OptionId{.value = optionId});
    REQUIRE(form.ready());
    form.session().submit();
    REQUIRE(pumpUntil([&] { return form.session().lastError() != nullptr; }));
    CHECK_FALSE(client.vote.finalized());

    client.vote.setAdminTokenDraft(*poll.adminToken);
    client.vote.useAdminToken();
    form.set<&polls::FinalizePoll::optionId>(polls::OptionId{.value = optionId});
    form.session().submit();
    REQUIRE(pumpUntil([&] { return client.vote.finalized(); }));
    CHECK(client.vote.title() == "T  (finalized)");
    CHECK_FALSE(client.vote.canChangeVotes());
    CHECK_FALSE(client.vote.canFinalize());
    CHECK(client.vote.statusText() == "poll finalized");
}

TEST_CASE("UndoLastVoteChange puts the participant's previous votes back, all three backend modes",
          "[polls][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    VoteFixture client{mode};
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    polls::testing::openAndWait(client.vote, poll.pollId);
    std::int64_t const optionId = client.vote.options()[0].optionId;
    client.vote.setParticipantName("alice");
    voteFor(client.vote, optionId, polls::VoteChoice::Yes, "yes 1 · if need be 0 · no 0");
    voteFor(client.vote, optionId, polls::VoteChoice::No, "yes 0 · if need be 0 · no 1");

    auto& form = client.vote.undoForm();
    form.set<&polls::UndoLastVoteChange::participantName>(std::string{"alice"});
    REQUIRE(form.ready());
    form.session().submit();
    REQUIRE(pumpUntil([&] { return client.vote.options()[0].tally == "yes 1 · if need be 0 · no 0"; }));
    CHECK(client.vote.statusText() == "your last vote change is undone");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_polls_tests`
Expected: FAIL — `no member named 'commentForm' in 'polls::client::VoteController'`.

- [ ] **Step 3: Implement**

In `vote_controller.hpp`, add after `#include <morph/core/executor.hpp>`:

```cpp
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/handler_submitter.hpp>
#include <morph/forms/engine/typed_form.hpp>
```

and after `#include "controllers/poll_text.hpp"`: `#include "app/form_success.hpp"`. After `useAdminToken()`'s
declaration add:

```cpp
    /// @brief The comment form's type: typed, on the attached handler.
    using CommentForm = ::morph::forms::Form<AddComment, PollModel, ::morph::bridge::AllowShared>;
    /// @brief The finalize form's type.
    using FinalizeForm = ::morph::forms::Form<FinalizePoll, PollModel, ::morph::bridge::AllowShared>;
    /// @brief The undo form's type.
    using UndoForm = ::morph::forms::Form<UndoLastVoteChange, PollModel, ::morph::bridge::AllowShared>;

    /// @brief The AddComment form.
    /// @return It.
    [[nodiscard]] CommentForm& commentForm() noexcept { return _comment; }

    /// @brief The FinalizePoll form; `FinalizePoll` needs the admin token in the session.
    /// @return It.
    [[nodiscard]] FinalizeForm& finalizeForm() noexcept { return _finalize; }

    /// @brief The UndoLastVoteChange form.
    /// @return It.
    [[nodiscard]] UndoForm& undoForm() noexcept { return _undo; }

    /// @brief Whether the finalize form acts. Tracked.
    /// @return True while a poll is open and not finalized.
    [[nodiscard]] bool canFinalize() const { return _canChangeVotes.get(); }
```

Add `void onFormSucceeded(::morph::forms::FormSession& session, std::string outcome);` after
`void reportError(...)`, the members

```cpp
    CommentForm _comment;
    FinalizeForm _finalize;
    UndoForm _undo;
```

directly after `_update`, and directly before `::morph::reactive::Effect _issueOpen;`:

```cpp
    ::morph::examples::FormSuccess _onCommented;
    ::morph::examples::FormSuccess _onFinalized;
    ::morph::examples::FormSuccess _onUndone;
```

In `vote_controller.cpp`, after the `_update{…}` initialiser add:

```cpp
      _comment{wiring.runtime, _handler, ::morph::forms::handlerChoiceFetcher(wiring.callbacks, _handler)},
      _finalize{wiring.runtime, _handler, ::morph::forms::handlerChoiceFetcher(wiring.callbacks, _handler)},
      _undo{wiring.runtime, _handler, ::morph::forms::handlerChoiceFetcher(wiring.callbacks, _handler)},
```

and directly before the `_issueOpen{…}` initialiser:

```cpp
      _onCommented{wiring.runtime, _comment.session(),
                   [this] { onFormSucceeded(_comment.session(), "comment added"); }},
      _onFinalized{wiring.runtime, _finalize.session(),
                   [this] { onFormSucceeded(_finalize.session(), "poll finalized"); }},
      _onUndone{wiring.runtime, _undo.session(),
                [this] { onFormSucceeded(_undo.session(), "your last vote change is undone"); }},
```

and at the end of the file, before the namespace's closing brace:

```cpp
void VoteController::onFormSucceeded(::morph::forms::FormSession& session, std::string outcome) {
    _state.refetch();
    session.reset();
    _status.set(infoLine(std::move(outcome)));
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_polls_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/polls/ladder_polls_tests "[polls][controller]"
```

Expected: PASS. Mutation check: delete `_state.refetch();` from `onFormSucceeded`. Expected FAIL in "A comment
through its form shows in the comments" (the comment never appears). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/polls/app/controllers examples/polls/tests/test_vote_forms.cpp
git commit -m "wip(polls): the comment, finalize and undo forms, typed on the attached handler

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 21: The live activity feed — a `Poller` per open poll

Part 6's `examples::Poller` starts from a fixed cursor, so the vote controller holds one per open poll: an Effect
over the attachment starts a fresh poller at the attach's `lastEventId` and drops it when the user leaves.

**Files:**
- Modify: `examples/polls/app/controllers/vote_controller.{hpp,cpp}`
- Test: `examples/polls/tests/test_vote_activity.cpp`

**Interfaces:**
- Consumes: `examples::Poller<Event, Cursor>(reactive::Runtime&, reactive::Scheduler&, Fetch, Cursor start, OnEvent,
  PollerOptions)`, `Poller::Page` (`PollPage<Event, Cursor>{events, next}`), `stoppedBy()`,
  `PollerOptions{interval}` (`examples/common/app/poller.hpp`, Part 6); `examples::mapCompletion<To>(owner, token,
  from, onValue, onError)` (`examples/common/app/completion_map.hpp`, Part 6); `async::CallbackScope::token()`;
  `Wiring::{scheduler, callbacks}` (Task 19's constructor already takes them); `polls::{GetEventsSince,
  GetEventsSinceResult, PollEvent, PollEventId}`; `reactive::testing::ManualScheduler::advance`.
- Produces: `polls::client::ActivityRow`; `VoteController::{activity(), kActivityPeriod, kActivityRows}`.

- [ ] **Step 1: Write the failing test**

Create `examples/polls/tests/test_vote_activity.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <string>

#include "client_fixture.hpp"
#include "controllers/vote_controller.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using polls::client::VoteController;
using polls::testing::VoteFixture;

}  // namespace

TEST_CASE("The activity feed shows another participant's vote and refreshes the tallies, all three backend modes",
          "[polls][controller]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    VoteFixture client{mode};
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    polls::testing::openAndWait(client.vote, poll.pollId);
    std::int64_t const optionId = client.vote.options()[0].optionId;

    // Another participant, on a handler of its own attached to the same shared poll instance. This client
    // never votes, so the only way bob's vote reaches its tallies is the feed's refetch.
    morph::bridge::BridgeHandler<polls::PollModel, morph::bridge::AllowShared> other{client.rig->bridge(0),
                                                                                    client.rig->executor()};
    static_cast<void>(awaitQt(other.execute(polls::OpenPoll{.pollId = poll.pollId})));
    static_cast<void>(awaitQt(other.execute(polls::SubmitVotes{
        .participantName = "bob",
        .votes = {polls::OneVote{.optionId = polls::OptionId{.value = optionId}, .choice = polls::VoteChoice::Yes}}})));

    client.scheduler.advance(VoteController::kActivityPeriod);
    REQUIRE(pumpUntil([&] { return !client.vote.activity().empty(); }));
    CHECK(client.vote.activity().back().line.find("[vote]") != std::string::npos);
    CHECK(client.vote.activity().back().eventId > 0);
    REQUIRE(pumpUntil([&] { return client.vote.options()[0].tally == "yes 1 · if need be 0 · no 0"; }));
}

TEST_CASE("Leaving the poll empties the activity feed", "[polls][controller]") {
    DbFixture fixture;
    VoteFixture client{Mode::Local};
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    polls::testing::openAndWait(client.vote, poll.pollId);
    client.vote.setParticipantName("alice");
    client.vote.setPick(client.vote.options()[0].optionId, polls::VoteChoice::Yes);
    client.vote.vote();
    client.scheduler.advance(VoteController::kActivityPeriod);
    REQUIRE(pumpUntil([&] { return !client.vote.activity().empty(); }));

    client.vote.close();
    REQUIRE(pumpUntil([&] { return client.vote.activity().empty(); }));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_polls_tests`
Expected: FAIL — `no member named 'activity' in 'polls::client::VoteController'`.

- [ ] **Step 3: Implement**

In `vote_controller.hpp`, add the includes `<chrono>`, `<cstddef>`, `<morph/core/callback_scope.hpp>`,
`<morph/core/completion.hpp>`, `<morph/reactive/scheduler.hpp>` and `"app/poller.hpp"`. Before
`class VoteController`, add:

```cpp
/// @brief One event of the open poll's live activity feed.
struct ActivityRow {
    std::int64_t eventId = 0;  ///< The event; also the row's key.
    std::string line;          ///< "#12  [vote]  alice voted".

    /// @brief Field-wise equality.
    bool operator==(ActivityRow const&) const = default;
};
```

Append to the class doc comment: "The activity feed is an `examples::Poller` over `GetEventsSince` on the same
handler, one per open poll, started at the attach's `lastEventId`; an applied event refetches the state, because
an event carries a summary, not a tally delta." The constructor is unchanged: `wiring.scheduler` paces the feed.

After `useAdminToken()`'s declaration (Task 20's form accessors follow it) add:

```cpp
    /// @brief The open poll's live activity, newest last, at most `kActivityRows`. Tracked.
    /// @return The rows.
    [[nodiscard]] std::vector<ActivityRow> const& activity() const { return _activity.get(); }

    /// @brief How often the activity feed asks for new events.
    static constexpr std::chrono::milliseconds kActivityPeriod{3000};
    /// @brief How many activity rows are kept; the tallies, not this log, are the source of truth.
    static constexpr std::size_t kActivityRows = 200;
```

In the private section, after the `Attachment` struct:

```cpp
    using ActivityPoller = ::morph::examples::Poller<PollEvent, PollEventId>;

    void startFeed(PollEventId from);
    void stopFeed();
    void onEvent(PollEvent const& event);
    [[nodiscard]] ::morph::async::Completion<ActivityPoller::Page> eventsSince(PollEventId since);
```

Members — add `::morph::exec::IExecutor* _callbacks;` and `::morph::reactive::Scheduler* _scheduler;` directly after
`_bridge`; directly after `_status`:

```cpp
    // Bumped whenever `_feed` is replaced, so a reader of `_feed` re-reads the new one.
    ::morph::reactive::Signal<std::uint64_t> _feedGeneration;
    ::morph::reactive::Signal<std::vector<PollEvent>> _events;
    // Counts applied events since the feed started; each change refetches the state once per flush.
    ::morph::reactive::Signal<std::uint64_t> _eventCount;
```

`std::optional<ActivityPoller> _feed;` directly after `_undo` (Task 20);
`::morph::reactive::Computed<std::vector<ActivityRow>> _activity;` directly after `_canUseAdminToken`;
`::morph::reactive::Effect _onEvents;` and `::morph::reactive::Effect _onFeedStopped;` directly after
`_onUpdateFailed`; `::morph::reactive::Effect _followAttachment;` directly before `_issueOpen`; and, after
`_issueOpen`, as the last member:

```cpp
    // Last: it gates the GetEventsSince continuations `eventsSince` attaches.
    ::morph::async::CallbackScope _lifetime;
```

In `vote_controller.cpp`, add `#include "app/completion_map.hpp"`; add the initialisers
`_callbacks{&wiring.callbacks},` and `_scheduler{&wiring.scheduler},` after `_bridge{&wiring.bridge},`;
`_feedGeneration{wiring.runtime, 0},`, `_events{wiring.runtime, std::vector<PollEvent>{}},` and
`_eventCount{wiring.runtime, 0},` after `_status{…},`;

```cpp
      _activity{wiring.runtime,
                [this] {
                    std::vector<ActivityRow> rows;
                    for (auto const& event : _events.get()) {
                        rows.push_back(ActivityRow{.eventId = *event.id,
                                                   .line = "#" + std::to_string(*event.id) + "  [" + event.kind +
                                                           "]  " + event.summary});
                    }
                    return rows;
                }},
```

after `_canUseAdminToken{…},`;

```cpp
      _onEvents{wiring.runtime,
                [this] {
                    if (_eventCount.get() > 0) {
                        _rt->untracked([this] { _state.refetch(); });
                    }
                }},
      _onFeedStopped{wiring.runtime,
                     [this] {
                         static_cast<void>(_feedGeneration.get());
                         auto const error = _feed.has_value() ? _feed->stoppedBy() : std::exception_ptr{};
                         if (error != nullptr) {
                             _status.set(StatusLine{.text = "live updates stopped: " +
                                                            ::morph::reactive::errorMessage(error),
                                                    .isError = true});
                         }
                     }},
```

after `_onUpdateFailed{…},`; and directly before `_issueOpen{…}`:

```cpp
      _followAttachment{wiring.runtime,
                        [this] {
                            auto const& attachment = _attached.get();
                            std::optional<PollEventId> const from =
                                attachment.has_value() ? std::optional<PollEventId>{attachment->cursor} : std::nullopt;
                            _rt->untracked([&] {
                                if (from.has_value()) {
                                    startFeed(*from);
                                } else {
                                    stopFeed();
                                }
                            });
                        }},
```

Add at the end of the file:

```cpp
void VoteController::startFeed(PollEventId from) {
    _feed.reset();
    _rt->batch([this] {
        _events.set({});
        _eventCount.set(0);
    });
    _feed.emplace(*_rt, *_scheduler, [this](PollEventId const& since) { return eventsSince(since); }, from,
                  [this](PollEvent const& event) { onEvent(event); },
                  ::morph::examples::PollerOptions{.interval = kActivityPeriod});
    _feedGeneration.set(_feedGeneration.peek() + 1);
}

void VoteController::stopFeed() {
    if (!_feed.has_value() && _events.peek().empty()) {
        return;
    }
    _feed.reset();
    _rt->batch([this] {
        _events.set({});
        _eventCount.set(0);
        _feedGeneration.set(_feedGeneration.peek() + 1);
    });
}

void VoteController::onEvent(PollEvent const& event) {
    _events.mutate([&event](std::vector<PollEvent>& events) {
        events.push_back(event);
        if (events.size() > kActivityRows) {
            events.erase(events.begin());
        }
    });
    _eventCount.set(_eventCount.peek() + 1);
}

::morph::async::Completion<VoteController::ActivityPoller::Page> VoteController::eventsSince(PollEventId since) {
    using Page = ActivityPoller::Page;
    // The page is the reply's events and the cursor after them; a reply with none keeps the asked cursor.
    return ::morph::examples::mapCompletion<Page>(
        *_callbacks, _lifetime.token(), _handler.execute(GetEventsSince{.lastEventId = since}),
        [since](GetEventsSinceResult const& result) {
            return Page{.events = result.events, .next = result.events.empty() ? since : result.events.back().id};
        },
        [](std::exception_ptr const&) {});
}
```

`startFeed`, `stopFeed`, `eventsSince` and `_onFeedStopped` are the only places that name the `Poller` surface.

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_polls_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/polls/ladder_polls_tests "[polls][controller]"
```

Expected: PASS — and every earlier `[polls][controller]` case still passes with a feed running whenever a poll is
open. Mutation check: in `_onEvents`, delete the `_rt->untracked([this] { _state.refetch(); });` call. Expected
FAIL in "The activity feed shows another participant's vote and refreshes the tallies" (the last wait times out:
this client never re-reads otherwise). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/polls/app/controllers examples/polls/tests/client_fixture.hpp examples/polls/tests/test_vote_activity.cpp
git commit -m "wip(polls): the live activity feed, a Poller per open poll

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 22: Navigation and the three screens' views

**Files:**
- Create: `examples/polls/app/controllers/polls_navigator.{hpp,cpp}`, `examples/polls/app/views/poll_views.{hpp,cpp}`
- Test: `examples/polls/tests/test_polls_navigator.cpp`, `examples/polls/tests/test_poll_views.cpp`

**Interfaces:**
- Consumes: the four controllers (Tasks 18–21); Part 2's `ui` builders (`grid`, `select`, `textInput`,
  `forEach` over a `Signal`, `switchOn<E>`), `ui::Mounted`, `RecordingBackend` (`find`, `all`, `prop`, `exists`,
  `click`, `edit`, `choose`); `forms::formView`.
- Produces: `polls::client::{Route, PollsNavigator}` — `PollsNavigator(Runtime&, VoteController&,
  CreatePollController&, CreateAccess)` with `route()`, `createOffered()`, `openPoll()`, `showCreate()`,
  `showLanding()`; `polls::client::{landingScreen, createScreen, voteScreen, pollsScreen}`.

- [ ] **Step 1: Write the failing tests**

Create `examples/polls/tests/test_polls_navigator.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include "client_fixture.hpp"
#include "controllers/create_poll_controller.hpp"
#include "controllers/polls_navigator.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using polls::client::CreateAccess;
using polls::client::Route;

}  // namespace

TEST_CASE("The navigator routes landing, create, vote and back, closing the vote screen on the way out",
          "[polls][controller]") {
    DbFixture fixture;
    polls::testing::VoteFixture client{Mode::Local};
    polls::client::CreatePollController create{client.runtime, client.rig->bridge(0), *client.rig->executor()};
    polls::client::PollsNavigator navigator{client.runtime, client.vote, create, CreateAccess::Offered};
    CHECK(navigator.route() == Route::Landing);
    CHECK(navigator.createOffered());

    navigator.showCreate();
    CHECK(navigator.route() == Route::Create);
    auto const poll = polls::testing::seedPoll(*client.rig, "T", {"1", "2"});
    navigator.openPoll("  " + poll.pollId + "  ");
    CHECK(navigator.route() == Route::Vote);
    REQUIRE(pumpUntil([&] { return client.vote.attached(); }));

    navigator.showLanding();
    CHECK(navigator.route() == Route::Landing);
    CHECK_FALSE(client.vote.attached());

    navigator.openPoll("   ");
    CHECK(navigator.route() == Route::Landing);  // a blank id opens nothing
}

TEST_CASE("With create hidden, the create screen is unreachable", "[polls][controller]") {
    DbFixture fixture;
    polls::testing::VoteFixture client{Mode::Local};
    polls::client::CreatePollController create{client.runtime, client.rig->bridge(0), *client.rig->executor()};
    polls::client::PollsNavigator navigator{client.runtime, client.vote, create, CreateAccess::Hidden};
    CHECK_FALSE(navigator.createOffered());
    navigator.showCreate();
    CHECK(navigator.route() == Route::Landing);
}
```

Create `examples/polls/tests/test_poll_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "client_fixture.hpp"
#include "controllers/create_poll_controller.hpp"
#include "controllers/landing_controller.hpp"
#include "controllers/polls_navigator.hpp"
#include "testkit/db_fixture.hpp"
#include "views/poll_views.hpp"

namespace {

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using morph::ui::Key;
using morph::ui::Mounted;
using morph::ui::testing::RecordingBackend;
namespace client = polls::client;

/// @brief Every widget of @p kind whose @p prop is @p value.
[[nodiscard]] std::vector<int> allWith(RecordingBackend const& backend, std::string const& kind,
                                       std::string const& prop, std::string const& value) {
    std::vector<int> found;
    for (int const widget : backend.all(kind)) {
        if (backend.prop(widget, prop) == value) {
            found.push_back(widget);
        }
    }
    return found;
}

/// @brief One client with all four controllers and the navigator, as the application wires them.
struct ScreenFixture {
    explicit ScreenFixture(client::CreateAccess access)
        : vote{Mode::Local},
          landing{vote.runtime},
          create{vote.runtime, vote.rig->bridge(0), *vote.rig->executor()},
          navigator{vote.runtime, vote.vote, create, access} {}

    polls::testing::VoteFixture vote;
    client::LandingController landing;
    client::CreatePollController create;
    client::PollsNavigator navigator;
};

}  // namespace

TEST_CASE("landingScreen: the tree, the Open gate, and create offered only where it is", "[polls][view]") {
    DbFixture fixture;
    for (auto const access : {client::CreateAccess::Offered, client::CreateAccess::Hidden}) {
        ScreenFixture screen{access};
        RecordingBackend backend;
        Mounted const mounted{screen.vote.runtime, backend, client::landingScreen(screen.landing, screen.navigator)};
        REQUIRE(pumpUntil([&] { return backend.find("Button", "label", "Open").has_value(); }));
        auto const open = *backend.find("Button", "label", "Open");
        CHECK(backend.prop(open, "enabled") == "false");
        auto const createButton = backend.find("Button", "label", "Create a new poll (organizer)");
        REQUIRE(createButton.has_value());
        CHECK(backend.prop(*createButton, "visible") == (access == client::CreateAccess::Offered ? "true" : "false"));

        auto const field = backend.find("TextInput", "placeholder", "poll id");
        REQUIRE(field.has_value());
        backend.edit(*field, "  abc  ");
        REQUIRE(pumpUntil([&] { return backend.prop(open, "enabled") == "true"; }));
        backend.click(open);
        REQUIRE(pumpUntil([&] { return screen.navigator.route() == client::Route::Vote; }));
    }
}

TEST_CASE("createScreen: rows follow the drafts, keep their widgets on a removal, and create a poll",
          "[polls][view]") {
    DbFixture fixture;
    ScreenFixture screen{client::CreateAccess::Offered};
    RecordingBackend backend;
    Mounted const mounted{screen.vote.runtime, backend, client::createScreen(screen.create, screen.navigator)};
    REQUIRE(pumpUntil([&] { return allWith(backend, "TextInput", "placeholder", "e.g. 2026-09-01").size() == 2; }));

    auto const add = *backend.find("Button", "label", "+ add option");
    backend.click(add);
    REQUIRE(pumpUntil([&] { return allWith(backend, "TextInput", "placeholder", "e.g. 2026-09-01").size() == 3; }));
    auto const rows = allWith(backend, "TextInput", "placeholder", "e.g. 2026-09-01");
    auto const removes = allWith(backend, "Button", "label", "remove");
    REQUIRE(removes.size() == 3);
    backend.click(removes[1]);
    REQUIRE(pumpUntil([&] { return allWith(backend, "TextInput", "placeholder", "e.g. 2026-09-01").size() == 2; }));
    CHECK(backend.exists(rows[0]));
    CHECK_FALSE(backend.exists(rows[1]));
    CHECK(backend.exists(rows[2]));  // the surviving rows kept their widgets

    backend.edit(*backend.find("TextInput", "placeholder", "e.g. Team offsite"), "Team offsite");
    backend.edit(rows[0], "2026-09-01");
    backend.edit(rows[2], "2026-09-02");
    auto const submit = *backend.find("Button", "label", "Create poll");
    REQUIRE(pumpUntil([&] { return backend.prop(submit, "enabled") == "true"; }));
    backend.click(submit);
    REQUIRE(pumpUntil([&] { return screen.create.created(); }));
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", screen.create.pollId()).has_value(); }));
    CHECK(backend.find("Button", "label", "Open this poll now").has_value());
}

TEST_CASE("voteScreen: the tree, a radio pick and the vote button drive the controller", "[polls][view]") {
    DbFixture fixture;
    ScreenFixture screen{client::CreateAccess::Offered};
    auto const poll = polls::testing::seedPoll(*screen.vote.rig, "Team offsite", {"1", "2"});
    RecordingBackend backend;
    Mounted const mounted{screen.vote.runtime, backend, client::voteScreen(screen.vote.vote, screen.navigator)};
    polls::testing::openAndWait(screen.vote.vote, poll.pollId);
    REQUIRE(pumpUntil([&] { return backend.all("Select").size() == 2; }));
    CHECK(backend.find("Text", "text", "Team offsite").has_value());
    CHECK(backend.find("Text", "text", "Live activity").has_value());
    CHECK(backend.find("Button", "label", "Comment").has_value());
    CHECK(backend.find("Button", "label", "Finalize").has_value());

    std::int64_t const optionId = screen.vote.vote.options()[0].optionId;
    backend.choose(backend.all("Select").front(), Key{std::string{"Yes"}});
    REQUIRE(pumpUntil([&] { return screen.vote.vote.pickFor(optionId) == polls::VoteChoice::Yes; }));
    backend.edit(*backend.find("TextInput", "placeholder", "participant name"), "alice");
    auto const voteButton = *backend.find("Button", "label", "Submit my votes");
    REQUIRE(pumpUntil([&] { return backend.prop(voteButton, "enabled") == "true"; }));
    backend.click(voteButton);
    REQUIRE(pumpUntil([&] { return backend.find("Text", "text", "yes 1 · if need be 0 · no 0").has_value(); }));
    CHECK(backend.prop(voteButton, "label") == "Update my votes");
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/all --target ladder_polls_tests`
Expected: FAIL — `'controllers/polls_navigator.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/polls/app/controllers/polls_navigator.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <string>

#include "controllers/create_poll_controller.hpp"
#include "controllers/landing_controller.hpp"
#include "controllers/vote_controller.hpp"

/// @file
/// Which of polls' three screens shows, and the moves between them.

namespace polls::client {

/// @brief polls' screens.
enum class Route : std::uint8_t {
    Landing,  ///< Open a poll by id, or (native) create one.
    Create,   ///< The organizer's create-poll screen.
    Vote,     ///< One open poll.
};

/// @brief The navigation controller: the route, and what each move does to the screens it leaves and enters.
class PollsNavigator {
public:
    /// @param runtime The runtime.
    /// @param vote The vote screen's controller. Borrowed.
    /// @param create The create screen's controller. Borrowed.
    /// @param access Whether this build may create polls.
    PollsNavigator(::morph::reactive::Runtime& runtime, VoteController& vote, CreatePollController& create,
                   CreateAccess access);

    ~PollsNavigator() = default;
    PollsNavigator(PollsNavigator const&) = delete;
    PollsNavigator& operator=(PollsNavigator const&) = delete;
    PollsNavigator(PollsNavigator&&) = delete;
    PollsNavigator& operator=(PollsNavigator&&) = delete;

    /// @brief The screen showing. Tracked.
    /// @return The route.
    [[nodiscard]] Route route() const { return _route.get(); }

    /// @brief Whether the landing screen offers "Create a new poll".
    /// @return True for `CreateAccess::Offered`.
    [[nodiscard]] bool createOffered() const noexcept { return _access == CreateAccess::Offered; }

    /// @brief Opens a poll on the vote screen; a blank id does nothing.
    /// @param pollId The poll's id, blanks allowed around it.
    void openPoll(std::string const& pollId);

    /// @brief Shows an empty create screen; refused when creating is not offered.
    void showCreate();

    /// @brief Back to the landing screen, leaving any open poll.
    void showLanding();

private:
    VoteController* _vote;
    CreatePollController* _create;
    CreateAccess _access;
    ::morph::reactive::Signal<Route> _route;
};

}  // namespace polls::client
```

Create `examples/polls/app/controllers/polls_navigator.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "controllers/polls_navigator.hpp"

#include <utility>

#include "controllers/poll_text.hpp"

namespace polls::client {

PollsNavigator::PollsNavigator(::morph::reactive::Runtime& runtime, VoteController& vote, CreatePollController& create,
                               CreateAccess access)
    : _vote{&vote}, _create{&create}, _access{access}, _route{runtime, Route::Landing} {}

void PollsNavigator::openPoll(std::string const& pollId) {
    std::string poll = trimmed(pollId);
    if (poll.empty()) {
        return;
    }
    _route.set(Route::Vote);
    _vote->open(std::move(poll));
}

void PollsNavigator::showCreate() {
    if (!createOffered()) {
        return;
    }
    _create->startOver();
    _route.set(Route::Create);
}

void PollsNavigator::showLanding() {
    _vote->close();
    _route.set(Route::Landing);
}

}  // namespace polls::client
```

Create `examples/polls/app/views/poll_views.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/view.hpp>

#include "controllers/create_poll_controller.hpp"
#include "controllers/landing_controller.hpp"
#include "controllers/polls_navigator.hpp"
#include "controllers/vote_controller.hpp"

/// @file
/// polls' three screens as view trees: bindings over the controllers, and nothing else. Every controller
/// passed in is borrowed and must outlive the mounted tree.

namespace polls::client {

/// @brief The landing screen: open a poll by id; create one where that is offered.
/// @param landing The landing controller.
/// @param navigator The navigation controller.
/// @return The screen.
[[nodiscard]] ::morph::ui::Node landingScreen(LandingController& landing, PollsNavigator& navigator);

/// @brief The create-poll screen: the title, the option drafts (a keyed `forEach` over their signal), and
///        the created poll's ids.
/// @param create The create controller.
/// @param navigator The navigation controller.
/// @return The screen.
[[nodiscard]] ::morph::ui::Node createScreen(CreatePollController& create, PollsNavigator& navigator);

/// @brief The vote screen: the vote grid (one row of radio choices per option), comments, admin, the three
///        forms and the live activity feed.
/// @param vote The vote controller.
/// @param navigator The navigation controller.
/// @return The screen.
[[nodiscard]] ::morph::ui::Node voteScreen(VoteController& vote, PollsNavigator& navigator);

/// @brief The whole client: a `Switch` over the navigator's route.
/// @param navigator The navigation controller.
/// @param landing The landing controller.
/// @param create The create controller.
/// @param vote The vote controller.
/// @return The root node.
[[nodiscard]] ::morph::ui::Node pollsScreen(PollsNavigator& navigator, LandingController& landing,
                                            CreatePollController& create, VoteController& vote);

}  // namespace polls::client
```

Create `examples/polls/app/views/poll_views.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "views/poll_views.hpp"

#include <morph/forms/engine/form_view.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "controllers/poll_text.hpp"

namespace polls::client {

namespace {

namespace ui = ::morph::ui;

[[nodiscard]] ui::Node form(::morph::forms::FormSession& session, std::string submitLabel) {
    return ::morph::forms::formView(session, {.overrides = {}, .gridColumns = 1, .submitLabel = std::move(submitLabel)});
}

template <class Controller>
[[nodiscard]] ui::Node statusOf(Controller& controller) {
    return ui::text({.text = [&controller] { return controller.statusText(); },
                     .role = [&controller] {
                         return controller.statusIsError() ? ui::TextRole::Error : ui::TextRole::Success;
                     },
                     .common = {.visible = [&controller] { return controller.hasStatus(); }}});
}

[[nodiscard]] std::vector<ui::SelectOption> choiceOptions() {
    std::vector<ui::SelectOption> options;
    for (VoteChoice const choice : {VoteChoice::Yes, VoteChoice::IfNeedBe, VoteChoice::No}) {
        options.push_back(ui::SelectOption{.key = ui::Key{choiceName(choice)}, .label = choiceLabel(choice)});
    }
    return options;
}

[[nodiscard]] ui::Node optionGrid(VoteController& vote, ::morph::reactive::Signal<OptionRow> const& row) {
    return ui::grid(
        {.columns = 3,
         .cells = {ui::GridCell{.node = ui::text({.text = [&row] { return row.get().label; }})},
                   ui::GridCell{.node = ui::text({.text = [&row] { return row.get().tally; },
                                                  .role = ui::TextRole::Muted})},
                   ui::GridCell{.node = ui::select(
                                    {.options = choiceOptions(),
                                     .selected =
                                         [&vote, &row] {
                                             return std::optional<ui::Key>{
                                                 ui::Key{choiceName(vote.pickFor(row.get().optionId))}};
                                         },
                                     .onSelect =
                                         [&vote, &row](ui::Key const& key) {
                                             if (auto const* name = std::get_if<std::string>(&key)) {
                                                 vote.setPick(row.peek().optionId, choiceNamed(*name));
                                             }
                                         },
                                     .style = ui::SelectStyle::Radio,
                                     .common = {.enabled = [&vote] { return vote.canChangeVotes(); }}})}},
         .gap = 2});
}

[[nodiscard]] ui::Node backRow(PollsNavigator& navigator, ui::Prop<std::string> heading) {
    return ui::row({.children = {ui::button({.label = "< Back", .onClick = [&navigator] { navigator.showLanding(); }}),
                                 ui::text({.text = std::move(heading), .role = ui::TextRole::Heading})},
                    .gap = 2});
}

}  // namespace

::morph::ui::Node landingScreen(LandingController& landing, PollsNavigator& navigator) {
    return ui::column(
        {.children = {ui::text({.text = "Doodle-style scheduling polls", .role = ui::TextRole::Heading}),
                      ui::text({.text = "Open a poll (paste the shared link's id)"}),
                      ui::row({.children = {ui::textInput({.value = [&landing] { return landing.pollIdDraft(); },
                                                           .onChange = [&landing](std::string text) {
                                                               landing.setPollIdDraft(std::move(text));
                                                           },
                                                           .onSubmit = [&landing, &navigator](std::string const&) {
                                                               navigator.openPoll(landing.trimmedPollId());
                                                           },
                                                           .placeholder = "poll id",
                                                           .common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                            ui::button({.label = "Open",
                                                        .onClick = [&landing, &navigator] {
                                                            navigator.openPoll(landing.trimmedPollId());
                                                        },
                                                        .common = {.enabled = [&landing] { return landing.canOpen(); }}})},
                               .gap = 1}),
                      ui::button({.label = "Create a new poll (organizer)",
                                  .onClick = [&navigator] { navigator.showCreate(); },
                                  .common = {.visible = [&navigator] { return navigator.createOffered(); }}})},
         .gap = 1});
}

::morph::ui::Node createScreen(CreatePollController& create, PollsNavigator& navigator) {
    ui::Node const draftRows = ui::forEach<OptionDraft>(
        create.options(), [](OptionDraft const& draft) { return ui::Key{draft.key}; },
        [&create](::morph::reactive::Signal<OptionDraft> const& draft) {
            return ui::row(
                {.children = {ui::textInput({.value = [&draft] { return draft.get().label; },
                                             .onChange = [&create, &draft](std::string text) {
                                                 create.setOptionLabel(draft.peek().key, std::move(text));
                                             },
                                             .onSubmit = {},
                                             .placeholder = "e.g. 2026-09-01",
                                             .common = {.layout = {.width = ui::Sizing::stretch()}}}),
                              ui::button({.label = "remove",
                                          .onClick = [&create, &draft] { create.removeOption(draft.peek().key); },
                                          .common = {.enabled = [&create] { return create.canRemove(); }}})},
                 .gap = 1});
        });
    ui::Node const editor = ui::panel(
        {.title = "New poll",
         .padding = 1,
         .child = ui::column(
             {.children = {ui::text({.text = "Title"}),
                           ui::textInput({.value = [&create] { return create.title(); },
                                          .onChange = [&create](std::string text) { create.setTitle(std::move(text)); },
                                          .onSubmit = {},
                                          .placeholder = "e.g. Team offsite"}),
                           ui::text({.text = "Candidate dates/options (2-20)"}), draftRows,
                           ui::button({.label = "+ add option",
                                       .onClick = [&create] { create.addOption(); },
                                       .common = {.enabled = [&create] { return create.canAdd(); }}}),
                           ui::button({.label = "Create poll",
                                       .onClick = [&create] { create.submit(); },
                                       .common = {.enabled = [&create] { return create.canSubmit(); }}})},
              .gap = 1}),
         .common = {.visible = [&create] { return create.editing(); }}});
    ui::Node const result = ui::panel(
        {.title = "Poll created",
         .padding = 1,
         .child = ui::column(
             {.children = {ui::text({.text = "Poll id (share it with the participants):"}),
                           ui::text({.text = [&create] { return create.pollId(); }}),
                           ui::text({.text = "Admin token (keep it: finalizing the poll needs it):"}),
                           ui::text({.text = [&create] { return create.adminToken(); }}),
                           ui::text({.text = "Participant token (goes out with the shared link):"}),
                           ui::text({.text = [&create] { return create.participantToken(); }}),
                           ui::button({.label = "Open this poll now",
                                       .onClick = [&create, &navigator] { navigator.openPoll(create.pollId()); }})},
              .gap = 1}),
         .common = {.visible = [&create] { return create.created(); }}});
    return ui::column({.children = {backRow(navigator, "Create a poll"), statusOf(create), editor, result}, .gap = 1});
}

::morph::ui::Node voteScreen(VoteController& vote, PollsNavigator& navigator) {
    ui::Node const votes = ui::column(
        {.children = {ui::text({.text = "Your name"}),
                      ui::textInput({.value = [&vote] { return vote.participantName(); },
                                     .onChange = [&vote](std::string text) { vote.setParticipantName(std::move(text)); },
                                     .onSubmit = {},
                                     .placeholder = "participant name"}),
                      ui::text({.text = "Options", .role = ui::TextRole::Heading}),
                      ui::forEach<OptionRow>(
                          [&vote] { return vote.options(); }, [](OptionRow const& row) { return ui::Key{row.optionId}; },
                          [&vote](::morph::reactive::Signal<OptionRow> const& row) { return optionGrid(vote, row); }),
                      ui::button({.label = [&vote] { return vote.voteLabel(); },
                                  .onClick = [&vote] { vote.vote(); },
                                  .common = {.enabled = [&vote] { return vote.canVote(); }}}),
                      ui::panel({.title = "Undo my last change", .padding = 1, .child = form(vote.undoForm().session(), "Undo")})},
         .gap = 1,
         .common = {.layout = {.width = ui::Sizing::stretch()}}});
    ui::Node const talk = ui::column(
        {.children = {ui::text({.text = [&vote] { return vote.commentsHeading(); }, .role = ui::TextRole::Heading}),
                      ui::forEach<CommentRow>(
                          [&vote] { return vote.comments(); }, [](CommentRow const& row) { return ui::Key{row.position}; },
                          [](::morph::reactive::Signal<CommentRow> const& row) {
                              return ui::text({.text = [&row] { return row.get().line; }});
                          }),
                      ui::panel({.title = "Add a comment", .padding = 1, .child = form(vote.commentForm().session(), "Comment")}),
                      ui::text({.text = "Admin", .role = ui::TextRole::Heading}),
                      ui::row({.children = {ui::textInput({.value = [&vote] { return vote.adminTokenDraft(); },
                                                          .onChange = [&vote](std::string text) {
                                                              vote.setAdminTokenDraft(std::move(text));
                                                          },
                                                          .onSubmit = {},
                                                          .placeholder = "admin token",
                                                          .mode = ui::TextInputMode::Password,
                                                          .common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                            ui::button({.label = "use",
                                                        .onClick = [&vote] { vote.useAdminToken(); },
                                                        .common = {.enabled = [&vote] { return vote.canUseAdminToken(); }}})},
                               .gap = 1}),
                      ui::panel({.title = "Finalize",
                                 .padding = 1,
                                 .child = form(vote.finalizeForm().session(), "Finalize"),
                                 .common = {.enabled = [&vote] { return vote.canFinalize(); }}})},
         .gap = 1,
         .common = {.layout = {.width = ui::Sizing::stretch()}}});
    ui::Node const activity = ui::column(
        {.children = {ui::text({.text = "Live activity", .role = ui::TextRole::Heading}),
                      ui::forEach<ActivityRow>(
                          [&vote] { return vote.activity(); }, [](ActivityRow const& row) { return ui::Key{row.eventId}; },
                          [](::morph::reactive::Signal<ActivityRow> const& row) {
                              return ui::text({.text = [&row] { return row.get().line; }, .role = ui::TextRole::Muted});
                          })},
         .gap = 1,
         .common = {.layout = {.width = ui::Sizing::stretch()}}});
    return ui::column({.children = {backRow(navigator, [&vote] { return vote.title(); }), statusOf(vote),
                                    ui::row({.children = {votes, talk, activity},
                                             .gap = 2,
                                             .common = {.layout = {.height = ui::Sizing::stretch()}}})},
                       .gap = 1});
}

::morph::ui::Node pollsScreen(PollsNavigator& navigator, LandingController& landing, CreatePollController& create,
                              VoteController& vote) {
    return ui::column(
        {.children = {ui::text({.text = "polls", .role = ui::TextRole::Heading}),
                      ui::switchOn<Route>([&navigator] { return navigator.route(); },
                                          {{Route::Landing, landingScreen(landing, navigator)},
                                           {Route::Create, createScreen(create, navigator)},
                                           {Route::Vote, voteScreen(vote, navigator)}})},
         .gap = 1});
}

}  // namespace polls::client
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target ladder_polls_tests \
  && QT_QPA_PLATFORM=offscreen ./build/all/examples/polls/ladder_polls_tests "[polls][controller],[polls][view]"
```

Expected: PASS. Mutation check: in `createScreen`, key the option rows by position instead of draft key — replace
`[](OptionDraft const& draft) { return ui::Key{draft.key}; }` with a lambda returning a running index. Expected
FAIL in "createScreen: rows follow the drafts, keep their widgets on a removal…" (`CHECK(backend.exists(rows[2]))`:
the last row's widget is the one destroyed). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/polls/app examples/polls/tests/test_polls_navigator.cpp examples/polls/tests/test_poll_views.cpp
git commit -m "wip(polls): navigation and the landing, create-poll and vote screens

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 23: polls' application, its native binary beside the browser one, and the old client deleted

**Files:**
- Create: `examples/polls/app/polls_application.{hpp,cpp}`, `examples/polls/ui/main.cpp`
- Modify: `codecov.yml` — polls' two `ignore:` entries
- Delete: `examples/polls/gui/`, `examples/polls/gui_lib/`, `examples/polls/gui_wasm/`,
  `examples/polls/tests/{test_poll_presenter,test_poll_qml_bridges,test_gui_qml_smoke}.cpp`
- Test: `examples/polls/tests/smoke/test_polls_frontends.cpp` (binary `ladder_polls_smoke_tests`, C3)

**Interfaces:**
- Consumes: as Task 6, plus `AppEnvironment::pollId` (Part 6's `fromArgs` fills it from `--poll` natively and from
  the page url's `?poll=` in the browser build), `Connection::ready()`, `ui::AppContext::{runtime, scheduler}`,
  `examples::Wiring`, `polls::db::setup`; the controllers and `pollsScreen` (Tasks 18–22).
- Produces: `polls::client::PollsApplication(ui::AppContext&, examples::AppEnvironment const&)`,
  `polls::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&)` (the contract's convention:
  the local database setup and `CreateAccess` — offered natively, hidden in the browser build — are decided
  inside); the `polls` binary — native for the first time.

Claims of the deleted test files, and where each one now lives:

| Deleted case | Re-expressed as |
|---|---|
| presenter: createPoll then openPoll (3 modes) | `test_vote_controller.cpp` "The vote screen opens a poll the create screen made" |
| presenter: getPollState after openPoll (3 modes) | "refresh re-reads the same poll's state" |
| presenter: submitVotes tallies (3 modes) | "A vote is tallied, and a second one replaces it" |
| presenter: updateVotes replaces (3 modes) | same case |
| presenter: addComment (3 modes) | `test_vote_forms.cpp` "A comment through its form shows in the comments" |
| presenter: finalizePoll with the admin token (3 modes) | "FinalizePoll is refused without the admin token and finalizes with it" |
| presenter: undoLastVoteChange (3 modes) | "UndoLastVoteChange puts the participant's previous votes back" |
| presenter: getEventsSince (3 modes) | `test_vote_activity.cpp` "The activity feed shows another participant's vote…" |
| presenter: every validation-driven action routes its failure | create: `test_create_poll_controller.cpp` "canSubmit needs a title and every option label…"; vote: "A vote is tallied…" (no name: `canVote()` false, `vote()` issues nothing); forms: "The vote screen's three forms are … each explicit" (`ready()` false for what `validate()` refuses) |
| presenter: getPollState/getEventsSince never attached | `test_vote_controller.cpp` "Before a poll is open nothing is issued…" (both are keyed on the attachment: idle, not failed) |
| presenter: finalizePoll with no session | "FinalizePoll is refused without the admin token…" (its first half) |
| bridges: QML surface audit | dropped with the QML (spec 4 §7) |
| bridges: createPoll bag {pollId, adminToken, participantToken} | "The create screen creates a poll and shows its three ids" |
| bridges: createPoll with < 2 options fails | "option drafts: the bounds hold…" (fewer than two cannot be built) + "canSubmit needs…" |
| bridges: openPoll emits state; a bad id fails | "The vote screen opens a poll…" + "Opening an unknown poll reports the model's message and attaches nothing" |
| bridges: openPoll's attach threads through every later action | the vote, form and activity suites all run on the one attached handler; "FinalizePoll is refused…" and "UndoLastVoteChange…" run after votes on it |
| bridges: submitIfValid refuses an action outside the schema document | "The vote screen's three forms are AddComment, FinalizePoll and UndoLastVoteChange, each explicit" — there is no untyped submission path left to refuse through |
| bridges: the EventPoller applies a live event and refreshes state | "The activity feed shows another participant's vote and refreshes the tallies" |
| smoke: Main.qml, the create screen, the vote screen load; three Submit buttons | `tests/smoke/test_polls_frontends.cpp` (TUI and Qt Quick) and `test_poll_views.cpp` (each screen; "Comment", "Finalize", "Undo" buttons) |

- [ ] **Step 1: Write the failing test**

Create `examples/polls/tests/smoke/test_polls_frontends.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <morph/ui/frontend.hpp>

#include "app/app_environment.hpp"
#include "polls_application.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/frontend_smoke.hpp"

#if MORPH_EXAMPLE_HAS_TUI || MORPH_EXAMPLE_HAS_QT_QUICK

namespace {

/// @brief The application in local mode, `--db` naming the database `DbFixture` already prepared.
[[nodiscard]] morph::ui::ApplicationFactory pollsFactory() {
    return [](morph::ui::AppContext& ctx) {
        morph::examples::AppEnvironment env;
        env.db = morph::ladder::testkit::DbFixture::computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"));
        return polls::client::makeApplication(ctx, env);
    };
}

}  // namespace

#endif

#if MORPH_EXAMPLE_HAS_TUI
TEST_CASE("polls mounts and quits on the TUI", "[polls][frontend]") {
    morph::ladder::testkit::DbFixture const fixture;
    morph::examples::testing::runFrontendSmoke(pollsFactory(), morph::examples::testing::SmokeFrontend::Tui);
}
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
TEST_CASE("polls mounts and quits on Qt Quick", "[polls][frontend]") {
    morph::ladder::testkit::DbFixture const fixture;
    morph::examples::testing::runFrontendSmoke(pollsFactory(), morph::examples::testing::SmokeFrontend::QtQuick);
}
#endif
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target ladder_polls_smoke_tests`
Expected: FAIL — `'polls_application.hpp' file not found`.

- [ ] **Step 3: Implement the application and the binary**

Create `examples/polls/app/polls_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>

#include "app/app_environment.hpp"
#include "app/transport.hpp"
#include "controllers/create_poll_controller.hpp"
#include "controllers/landing_controller.hpp"
#include "controllers/polls_navigator.hpp"
#include "controllers/vote_controller.hpp"

/// @file
/// polls as a `ui::Application`.

namespace polls::client {

/// @brief polls' application: one connection, the landing, create and vote controllers, and the navigator.
///        A poll id in the environment (`--poll`, or a browser's `?poll=`) opens that poll at start-up. A native
///        build offers creating a poll; the browser build does not (the README's design decision 6).
class PollsApplication final : public ::morph::ui::Application {
public:
    /// @param ctx The frontend's context; its scheduler runs the vote screen's timers.
    /// @param env What the command line (or the browser build) asked for; without `--server` its `db` (else
    ///            `POLLS_DB`, else `polls.db` in the working directory) is the database set up and hosted.
    PollsApplication(::morph::ui::AppContext& ctx, ::morph::examples::AppEnvironment const& env);

    ~PollsApplication() override = default;
    PollsApplication(PollsApplication const&) = delete;
    PollsApplication& operator=(PollsApplication const&) = delete;
    PollsApplication(PollsApplication&&) = delete;
    PollsApplication& operator=(PollsApplication&&) = delete;

    /// @brief The client's root view.
    /// @return `pollsScreen()` over the controllers.
    [[nodiscard]] ::morph::ui::Node view() override;

private:
    std::unique_ptr<::morph::examples::Connection> _connection;
    LandingController _landing;
    CreatePollController _create;
    VoteController _vote;
    PollsNavigator _navigator;
};

/// @brief The application factory `ui/main.cpp` and the smoke tests share.
/// @param ctx The frontend's context.
/// @param env The environment.
/// @return The application.
[[nodiscard]] std::unique_ptr<::morph::ui::Application> makeApplication(::morph::ui::AppContext& ctx,
                                                                       ::morph::examples::AppEnvironment const& env);

}  // namespace polls::client
```

Create `examples/polls/app/polls_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "polls_application.hpp"

#include <cstdlib>
#include <string>

#include "app/wiring.hpp"
#include "views/poll_views.hpp"

#ifndef __EMSCRIPTEN__
#include "polls/db/database.hpp"
#endif

namespace polls::client {

namespace {

// Creating a poll is the organizer's, from a native client; a participant in a browser follows a shared link.
#ifdef __EMSCRIPTEN__
constexpr CreateAccess kCreateAccess = CreateAccess::Hidden;
#else
constexpr CreateAccess kCreateAccess = CreateAccess::Offered;
#endif

/// @brief How a client without `--server` prepares its database: `--db`, else `POLLS_DB`, else `polls.db` in the
///        working directory. The browser build is always remote and never calls it.
[[nodiscard]] ::morph::examples::LocalSetup localSetup() {
#ifdef __EMSCRIPTEN__
    return ::morph::examples::LocalSetup{};
#else
    return ::morph::examples::LocalSetup{.setupDatabase = [](std::string const& database) {
        if (!database.empty()) {
            db::setup(database);
            return;
        }
        char const* fromEnvironment = std::getenv("POLLS_DB");
        db::setup(fromEnvironment != nullptr ? fromEnvironment : "DRIVER=SQLite3;Database=polls.db;Timeout=5000");
    }};
#endif
}

}  // namespace

PollsApplication::PollsApplication(::morph::ui::AppContext& ctx, ::morph::examples::AppEnvironment const& env)
    : _connection{::morph::examples::connect(ctx, env, localSetup())},
      _landing{ctx.runtime()},
      _create{ctx.runtime(), _connection->bridge(), _connection->callbacks()},
      _vote{::morph::examples::Wiring{.runtime = ctx.runtime(),
                                      .scheduler = ctx.scheduler(),
                                      .bridge = _connection->bridge(),
                                      .callbacks = _connection->callbacks()},
            [this] { return _connection->ready().get(); }},
      _navigator{ctx.runtime(), _vote, _create, kCreateAccess} {
    if (env.pollId.has_value()) {
        _navigator.openPoll(*env.pollId);
    }
}

::morph::ui::Node PollsApplication::view() { return pollsScreen(_navigator, _landing, _create, _vote); }

std::unique_ptr<::morph::ui::Application> makeApplication(::morph::ui::AppContext& ctx,
                                                          ::morph::examples::AppEnvironment const& env) {
    return std::make_unique<PollsApplication>(ctx, env);
}

}  // namespace polls::client
```

Create `examples/polls/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

/// @file
/// polls' one binary, native and WebAssembly: it reads the environment, offers every frontend this build
/// has, lets `ui::selectFrontend` pick one, and hands the frontend the application factory.
///
/// @code
/// polls                                   # in-process: hosts PollModel over --db / POLLS_DB
/// polls --server ws://127.0.0.1:8767      # against ladder_polls_server
/// polls --poll <id>                       # straight to that poll's vote screen
/// @endcode
///
/// The browser build is always remote, against `MORPH_LADDER_POLLS_WASM_SERVER_URL` unless the page url names a
/// `?server=`; `AppEnvironment::fromArgs` reads the page url's `?poll=` into `pollId`, so a shared link opens
/// its poll. It does not offer creating a poll — an organizer creates from a native client, and participants
/// follow the shared link (README, design decision 6; `app/polls_application.cpp` decides it).

#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <string>
#include <vector>

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#endif
#if MORPH_EXAMPLE_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

#include "app/app_environment.hpp"
#include "polls_application.hpp"

int main(int argc, char** argv) {
    try {
        auto env = morph::examples::AppEnvironment::fromArgs(argc, argv);
#ifdef __EMSCRIPTEN__
        if (!env.server.has_value()) {
            env.server = std::string{MORPH_LADDER_POLLS_WASM_SERVER_URL};
        }
#endif
        std::vector<morph::ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
        built.push_back(morph::qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
        built.push_back(morph::tui::frontendOption());
#endif
        auto const frontend = morph::ui::selectFrontend(built, argc, argv);
        return frontend->run([&env](morph::ui::AppContext& ctx) { return polls::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "polls: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "polls: unknown error\n";
    }
    return 1;
}
```

- [ ] **Step 4: Delete the old client, its tests, and its coverage exclusions**

```bash
git rm -r -q examples/polls/gui examples/polls/gui_lib examples/polls/gui_wasm \
    examples/polls/tests/test_poll_presenter.cpp examples/polls/tests/test_poll_qml_bridges.cpp \
    examples/polls/tests/test_gui_qml_smoke.cpp
```

In `codecov.yml`, replace the `ignore:` entries `"examples/polls/gui/**"` and `"examples/polls/gui_wasm/**"` with
the one entry `"examples/polls/ui/**"`.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build/all --target ladder_polls_tests ladder_polls_smoke_tests polls ladder_polls_server
QT_QPA_PLATFORM=offscreen ./build/all/examples/polls/ladder_polls_tests "[polls]"
ctest --test-dir build/all -R '^polls\.smoke\.' --output-on-failure
ls examples/polls    # CMakeLists.txt README.md app include src tests ui
```

Expected: PASS; the `ctest` line runs both frontend cases. Mutation check: change
`PollsApplication::view()`'s body to `throw std::logic_error{"no view"};` (with `#include <stdexcept>`). Expected
FAIL in both `polls.smoke.` cases. Restore.

- [ ] **Step 6: Commit**

```bash
git add -A examples/polls codecov.yml
git commit -m "wip(polls): the application, a native binary beside the browser one, the QML client removed

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 24: polls' README

**Files:**
- Modify: `examples/polls/README.md` — the status paragraph, three design-decision sentences, two
  Definition-of-done bullets, and everything from the (duplicated) heading "The client, and its known gaps"
  to the end of the file

**Interfaces:** none.

- [ ] **Step 1: List the stale references**

```bash
grep -n 'gui_lib\|gui_wasm\|gui/main\.cpp\|gui/qml\|\.qml\|PollBridge\|PollPresenter\|PollFormsController\|DynamicForm\|EventPoller\|nativeClient' examples/polls/README.md
```

Expected: lines 53, 138, 160, 381–437 and 439–593 (the sections the next step replaces).

- [ ] **Step 2: Rewrite**

Replace the status paragraph (lines 3–10, "**Status: shipped** …" through "… per [`LADDER.md`](../LADDER.md)'s
discipline rule.") with:

```markdown
**Status: shipped** — every rung-3 task is complete; see [Definition of done](#definition-of-done) for what that
does and does not mean, and ["The client, and its known gaps"](#the-client-and-its-known-gaps--stated-rather-than-smoothed-over)
for what the client cannot reach. The client runs natively on the terminal UI and on Qt Quick; its browser build
is the same program, compile-gated in CI and never run here. Design decisions below were resolved in writing
before implementation began, per [`LADDER.md`](../LADDER.md)'s discipline rule.
```

In design decision 1, replace "is accordingly generated, stored, returned and shown by\n   `CreatePollView.qml` —"
with "is accordingly generated, stored, returned and shown by\n   the create screen (`app/views/poll_views.cpp`) —".
In design decision 6, replace "since the `nativeClient` gate\n   in `gui/qml/Main.qml` still points here." with
"since the browser build's\n   `CreateAccess::Hidden` (`app/polls_application.cpp`) still points here.", and replace
"it dispatches through\n     `PollPresenter::_creator`, a plain `NoSharing`" with "it dispatches through\n
`CreatePollController`'s handler, a plain `NoSharing`" (`\n` marks the file's own line break; keep its indentation).

In "Definition of done", replace the "Live demo" bullet's text after its first sentence (from "**Not satisfied.**"
to the end of the bullet) with:

```markdown
  The `polls` binary runs natively on the terminal UI and on Qt Quick, so the demo can be run; it is a manual
  check (the group's verification step records what was exercised), not an automated one. Beneath it,
  `tests/test_app.cpp` drives the remote backend end to end, the vote-controller suites drive every action
  through the one attached handler in all three backend modes — including another participant's vote reaching
  this client through the activity feed — and `tests/test_shared_instance_lifecycle.cpp` covers multi-handler
  convergence on one shared poll.
```

and replace the "event-polling helper" bullet's **Confirmed (Task 15)** paragraph with:

```markdown
  **Confirmed:** the client's activity feed is `examples::Poller<PollEvent, PollEventId>`
  (`examples/common/app/poller.hpp`), outside this rung: it names no `polls::` type and takes its fetch as a
  function, which is what lets kanban drive its own feed with it. `VoteController` is its consumer here.
```

Replace everything from the first "## The client, and its known gaps — stated rather than smoothed over" line to
the end of the file with:

```markdown
## The client, and its known gaps — stated rather than smoothed over

The client (`app/`, target `ladder_polls_app`) is toolkit-free; `ui/main.cpp` picks the terminal UI or Qt Quick.
Three screens, one controller each, and a navigator between them:

- **Landing** — paste a poll id and open it; on a native client, "Create a new poll (organizer)".
- **Create a poll** — a title and 2–20 option drafts. `CreatePoll::options` is a list of objects, which a schema
  form does not edit, so the drafts are a signal the view renders with a keyed `forEach`: each draft keeps its
  key across every add and remove, so removing one leaves the others' fields alone. The created poll's id,
  admin token and participant token show once.
- **Vote** — the vote grid (one row per option: label, tally, a radio choice of Yes / If need be / No), the
  participant's name and the Submit/Update button, comments, the admin token, and three forms —
  `AddComment`, `FinalizePoll`, `UndoLastVoteChange` — each a typed `forms::Form<A, PollModel, AllowShared>`
  with its own Submit button (`explicitSubmit`). The live activity feed polls `GetEventsSince` every three
  seconds; each applied event refetches the poll's state, because an event carries a summary, not a tally.

Every action on an open poll runs through **one** `BridgeHandler<PollModel, AllowShared>`: `OpenPoll` attaches
it to the poll's shared instance, and an action on any other handler would have no instance to run against. A
reply to an `OpenPoll` for a poll the user has already left is dropped. A browser following a `?poll=` link
opens its poll at start-up, while its socket may still be connecting — and a keyed attach is refused outright then
("disconnected") rather than queued, because it may re-point a live instance. The vote controller therefore issues
`OpenPoll` only once the connection reports ready (`examples::Connection::ready()`).

What the client shows differently from the QML client it replaced:

- It runs natively. The QML client was browser-only, so creating a poll — native-only by design decision 6 —
  had no client at all; the native `polls` binary is that client.
- The created poll's ids show as text, not as selectable read-only fields: a terminal has no selection to copy
  from, and the Qt Quick rendering follows the same view.
- A form shows its own reply or error; the screen's status line reports opening, votes and the feed.

Known gaps:

- **The finalize form asks for an option's number.** `FinalizePoll::optionId` is a plain `OptionId`, not a
  `morph::forms::Choice`, so the form takes the `#id` the vote grid shows rather than offering the options.
- **The results display resyncs on every applied event rather than applying an increment.** `PollEvent` carries
  no tally delta, so each tick with an event refetches `GetPollState` — correct and simple, one extra round trip
  per busy tick, worth revisiting if a later rung's event volume makes it not.
- **The browser build cannot create a poll at all** (design decision 6); a participant follows a link or pastes
  an id.
- **The browser build is compile-gated, not run.** `.github/workflows/wasm-ladder.yml` builds target `polls` in
  its Emscripten configure; its `?poll=` handling and the wait for a ready connection have never run in a
  browser here.
- **No admin-token persistence.** The token is installed as the bridge's default session for the rest of the
  process; reopening the client needs it pasted again.
- **The frontend smoke tests prove mounting, not behaviour** — the behavioural half is the controller and view
  suites plus a manual run against `ladder_polls_server`.
```

- [ ] **Step 3: Verify**

Re-run Step 1's `grep`. Expected: no output. Then markdownlint on the file. Expected: no findings.

- [ ] **Step 4: Commit**

```bash
git add examples/polls/README.md
git commit -m "wip(polls): README describes the native and browser client

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 25: Verify polls, the part as a whole, and squash the group

**Files:** none new.

- [ ] **Step 1: The three rungs' suites and the shared testkit's**

```bash
cmake --build build/all
ctest --test-dir build/all -L 'ladder-polls|ladder-bookmarks|ladder-pastebin|ladder-0' --output-on-failure
```

Expected: every test passes.

- [ ] **Step 2: The clients are toolkit-free**

```bash
grep -rnE '#include <Q|morph/(qt|tui|qt_quick)/' examples/pastebin/app examples/bookmarks/app examples/polls/app \
  && echo "toolkit include in app/" || true
for t in ladder_pastebin_app ladder_pastebin_lib ladder_bookmarks_app ladder_bookmarks_lib ladder_polls_app ladder_polls_lib; do
  own=$(ninja -C build/all -t commands "$t" | grep "CMakeFiles/$t.dir/" || true)
  m=$(printf '%s\n' "$own" | grep -c . || true)
  n=$(printf '%s\n' "$own" | grep -cE 'Qt6|QtCore|/qt/' || true)
  echo "$t: $m own compile lines, $n Qt-bearing"; test "$m" -gt 0 && test "$n" -eq 0
done
```

Expected: no `grep` output; all six targets `0`.

- [ ] **Step 3: The model tests are untouched**

```bash
git diff --exit-code master -- examples/polls/tests/test_app.cpp examples/polls/tests/test_poll_dto.cpp \
    examples/polls/tests/test_poll_model.cpp examples/polls/tests/test_polls_authorizer.cpp \
    examples/polls/tests/test_polls_payload_shape.cpp examples/polls/tests/test_polls_schema.cpp \
    examples/polls/tests/test_polls_types.cpp examples/polls/tests/test_shared_instance_lifecycle.cpp \
    examples/polls/tests/test_vote_event_dto.cpp examples/polls/tests/.clang-tidy
```

Expected: exit 0.

- [ ] **Step 4: Run the application by hand — the organizer-plus-participants demo the README's Definition of
  done names**

```bash
POLLS_PORT=8767 ./build/all/examples/polls/ladder_polls_server &
./build/all/examples/polls/polls --server ws://127.0.0.1:8767 --ui=qt     # organizer: create a poll
./build/all/examples/polls/polls --server ws://127.0.0.1:8767 --ui=tui --poll <id>   # participant 1
./build/all/examples/polls/polls --server ws://127.0.0.1:8767 --ui=tui --poll <id>   # participant 2
kill %1
```

Vote from both participants and watch each one's activity feed and tallies follow within a poll period; finalize
with the admin token on the organizer and see voting lock on the participants. Record what was exercised in the
squash body.

- [ ] **Step 5: Sanitizer**

```bash
cmake --preset clang-asan -DMORPH_BUILD_QT=ON -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON \
      -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=polls
cmake --build build/clang-asan --target ladder_polls_tests
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/examples/polls/ladder_polls_tests asan
QT_QPA_PLATFORM=offscreen ./build/clang-asan/examples/polls/ladder_polls_tests "[polls][controller],[polls][view]"
```

Expected: clean.

- [ ] **Step 6: clang-tidy over the changed lines** — CONTRIBUTING's recipe, file count printed and asserted.
  Expected: no findings in the three rungs' `app/`, `ui/`, `tests/`.

- [ ] **Step 7: WebAssembly** — the three browser builds are `wasm-ladder.yml`'s named targets now (`pastebin`,
  `bookmarks`, `polls`). With no Emscripten toolchain locally, push nothing yet: state in the squash body that the
  browser builds are verified by that workflow only, and confirm on the pull request's first CI run that its
  "Build the WASM-remote spike and every rung's WASM client" step built all three (the step's log names each
  target).

- [ ] **Step 8: Commit any fixes** (skip if none, and say so)

```bash
git add -A examples/polls examples/bookmarks examples/pastebin examples/common cmake
git commit -m "wip(polls): fixes from the verification gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

- [ ] **Step 9: Squash the group**

Follow the master plan's "Squashing a part" procedure with `key=polls` and this message:

```text
examples/polls: one app, any frontend

polls' client is a toolkit-free app library — landing, create-poll (a
keyed forEach over a signal of 2-20 option drafts), vote (a grid of radio
choices, typed AddComment/FinalizePoll/UndoLastVoteChange forms on the one
attached AllowShared handler, a Poller activity feed) and a navigator —
plus one binary that now runs natively as well as in the browser, where a
?poll= link opens its poll once the connection is ready (a keyed attach is
refused, not queued, while it connects). The QML client and its tests are gone.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure must show the history ending in `examples/pastebin: …`, `examples/bookmarks: …`,
`examples/polls: …`.
