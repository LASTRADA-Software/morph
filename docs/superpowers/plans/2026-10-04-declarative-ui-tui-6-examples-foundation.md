# Declarative UI, Part 6 — The Examples Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Give every example application a Qt-free foundation — environment, transport, ids, UUIDs, an event
poller, controller wiring and completion mapping, test waits and a frontend smoke harness — teach `morph_add_rung` the `app/` + `ui/` layout, and ship the
two non-rung examples `examples/tui/gallery` and `examples/tui/workout` on the injected frontend.

**Architecture:** `examples/common/app/` is a new STATIC target, `morph_ladder_app_common`, whose interface names
no toolkit: the Qt (`QtWebSocketBackend`) and `morph::net` (`SocketBackend`) transports are separate translation
units compiled in only when their option is on and linked PRIVATE. It is added from the root `CMakeLists.txt`
whenever the ladder, the bank example or examples/tui is enabled. Test helpers (`testkit/wait.hpp`,
`testkit/fake_app_context.hpp`, `testkit/frontend_smoke.hpp`) form `morph_example_testkit`; a new CMake module
`cmake/morph_example_app.cmake` builds an example's one binary against whichever frontends were built. One
framework seam is added: `morph::net::SocketBackend` implements `setConnectHandler`/`setDisconnectHandler`, so a
terminal client learns its connection state without blocking.

**Tech Stack:** C++23; morph core (`Bridge`, `LocalBackend`, `IoLoop`, `Completion`), `morph::reactive`
(Part 1), `morph::ui` (Part 2), `morph::tui` (Part 3), `morph::qt_quick` (Part 4), `morph::net`, `morph::qt`;
core-cpp's `core::tui` test doubles; Catch2 v3; CMake.

**Spec:** `docs/superpowers/specs/2026-10-04-examples-migration-design.md` (spec 4) §2, §3, §4, §6, §8 item 1;
`docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` (spec 1) §8.

This is **Part 6 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` commit convention every task below follows, and this part ends by squashing its `wip(examples-common)`
commits into one. The commit is **additions only**: `examples/common`'s Qt pieces (`gui/`, `qml_surface`,
`testkit_main.cpp`'s GUI branch) and `morph_add_rung`'s `gui/`, `gui_lib/`, `gui/qml/` and `gui_wasm/` conventions
stay untouched and keep building every rung; Part 10 removes them (`pump.hpp` stays, for the rig's Socket mode,
whose server is Qt) and rewrites
`examples/TESTING.md`, `IMPLEMENTATION.md` and `LADDER.md` — this part does not edit those three files.

## Global Constraints

- Part 1's constraints apply unchanged (C++23; SPDX line first, `#pragma once`; clang-tidy naming; present-tense
  comments with no history and no issue numbers; `-Weverything -Werror`; clang-tidy `WarningsAsErrors: "*"`).
- Nothing under `examples/common/app/` includes a Qt, `core::tui` or frontend header except
  `transport_qt.cpp` (Qt) and `transport_net.cpp` (`morph::net`); the target links those PRIVATE.
- Every macro tested with `#if` is defined to `0` or `1` by CMake (`-Wundef` is on): `MORPH_EXAMPLE_HAS_TUI`,
  `MORPH_EXAMPLE_HAS_QT_QUICK`, `MORPH_EXAMPLES_TRANSPORT_QT`, `MORPH_EXAMPLES_TRANSPORT_NET`.
- A `main` catches every exception (clang-tidy `bugprone-exception-escape` checks `main`) and returns 1 with the
  message on `std::cerr`.
- Example code lives in its own namespace (`morph::examples`, `morph::examples::testing`); an example
  application's client code lives in `<app>::client` (`gallery::client`, `workout::client`) and exposes
  `[[nodiscard]] std::unique_ptr<ui::Application> <app>::client::makeApplication(ui::AppContext& ctx,
  examples::AppEnvironment const& env)`, so every `ui/main.cpp` has the shape of spec 4 §3 (the contract's
  "Conventions for every example application").
- View tests read Part 2's `RecordingBackend` (`include/morph/ui/testing/recording_backend.hpp`) exactly as it is
  written. Kinds: `Text`, `Button`, `TextInput`, `Checkbox`, `Select`, `Menu`, `Grid`, `Spacer`, `Panel`, `Scroll`,
  `Busy`, `DateTimeInput`, `Slider`, `FilePicker`, `Slot`, `Tabs`, `Dialog`, `Table`; a stack is `Column` or `Row`
  by its axis, so a `ui::forEach` (vertical by default) mounts as a `Column` and a `Switch` as a `Slot`. Props are
  the setter's name without `set`, first letter lower-cased (`label`, `title`, `open`, `selected`, `placeholder`),
  except that a `TextInput`'s value is `text`; a bool reads `"true"`/`"false"`. The mount skips a `Common` setter
  whose value is the constant default (no `enabled`/`visible` prop on a plain widget) and always calls it for a
  binding. `chooseIndex` on a `Menu` runs that item's action; on a `Tabs` it marks the tab `selected`, then calls
  `onSelect`. A `Tabs` page mounts the first time it is selected; a `Dialog`'s content exists only while it is
  open. `find` returns the lowest live id that matches.
- clang-tidy's `readability-identifier-length` holds in every code block, tests included: no parameter, lambda
  parameter or local shorter than three characters except `i j k x y n N fn cb op` (parameters) and
  `i j k x y lk cb op fn` (variables); struct and class data members are exempt. Headers (`include/**` and every
  example `app/**` header), and the non-test `.cpp` files too, use `.at()` or iteration, never an unchecked
  `operator[]` on a container (`cppcoreguidelines-pro-bounds-avoid-unchecked-container-access`); `std::span`,
  which has no `at()` before C++26, is indexed after an explicit bounds test.
- No `CHANGELOG.md` entry for the examples; one `### Added` line for the `SocketBackend` seam (Task 1).
- Every example test directory carries a `.clang-tidy` subtracting only `bugprone-chained-comparison`, as
  `examples/common/testkit/.clang-tidy` does.
- Commits end with `Signed-off-by: Christian Parpart <christian@parpart.family>`.

## Review Focus

1. **A connect handler installed after the connection is already up** must still learn it, once — on a threaded
   transport the first connect can beat the installation (Task 1 test "a connect handler installed while
   connected is called once at installation").
2. **A value option given as the last argument** (`app --server`) must be refused, not read past `argv` (Task 2
   test "a value option without its value is refused").
3. **A connect notification arriving after its `Connection` is destroyed** must touch nothing (Task 5 test "a
   connect notification after the connection is gone touches nothing"; ASan observes).
4. **A refresh-timer refetch while an events page is in flight** must not apply that page's events twice (Task 7
   test "a refetch while a page is in flight applies its events once").
5. **A view that never reaches the screen** must make the frontend smoke fail with a non-zero exit, not pass
   because `run` returned (Task 8 test "an empty view times out instead of passing").

---

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/net/socket_backend.hpp` | `SocketBackend::setConnectHandler` / `setDisconnectHandler` (the one framework seam) |
| `tests/net/test_socket_backend.cpp` | Cases for the two handlers |
| `docs/spec/core/backend.md`, `CHANGELOG.md` | The seam's spec text and changelog line |
| `CMakeLists.txt` (root) | `include(cmake/morph_example_app.cmake)`; the "Example applications on an injected frontend" block |
| `cmake/morph_example_app.cmake` | `morph_example_frontends(<target>)`, `morph_add_example_ui(TARGET … SOURCES … LIBRARIES …)` |
| `cmake/morph_add_rung.cmake` | `app/*.cpp` → `ladder_<rung>_app`, `ui/*.cpp` → `<rung>`, `tests/smoke/*.cpp` → `ladder_<rung>_smoke_tests` |
| `examples/common/app/CMakeLists.txt` | `morph_ladder_app_common`, `morph_example_testkit`, the tests subdirectory |
| `examples/common/app/app_environment.{hpp,cpp}` | `AppEnvironment::fromArgs`, `detail::applyQuery` (WASM `?poll=`/`?server=`) |
| `examples/common/app/uuid.{hpp,cpp}` | `newUuid()` — RFC 4122 v4, lower-case |
| `examples/common/app/ids.hpp` | Strong id ↔ `std::string` / `ui::Key` |
| `examples/common/app/transport.{hpp,cpp}` | `TransportError`, `LocalSetup`, `Link`, `Connection`, `connect()` |
| `examples/common/app/detail/remote_backends.hpp` | Declarations of the two optional remote-backend factories |
| `examples/common/app/transport_qt.cpp` | `QtWebSocketBackend` factory (only with `MORPH_BUILD_QT`) |
| `examples/common/app/transport_net.cpp` | `SocketBackend` factory (only with `MORPH_BUILD_NET`, POSIX, native) |
| `examples/common/app/poller.hpp` | `PollPage`, `PollerOptions`, `Poller<Event, Cursor>` over a `Query` with `refreshEvery` |
| `examples/common/app/wiring.hpp` | `Wiring{runtime, scheduler, bridge, callbacks}` — what every controller is built from, all borrowed |
| `examples/common/app/completion_map.hpp` | `mapCompletion<To>` — derive a completion on the owner, gated by a token |
| `examples/common/testkit/wait.hpp` | `pumpUntil` (MainThreadExecutor, StepExecutor), `awaitOn` |
| `examples/common/testkit/fake_app_context.hpp` | `FakeAppContext` — a headless `ui::AppContext` for controller and view tests |
| `examples/common/testkit/frontend_smoke.hpp`, `testkit_src/frontend_smoke.cpp` | `SmokeFrontend`, `SmokeResult`, `smokeRun`, `runFrontendSmoke` |
| `examples/common/app/tests/` | `examples_common_app_tests`: every piece above |
| `examples/tui/CMakeLists.txt`, `README.md` | Registers gallery and workout; how to run them |
| `examples/tui/gallery/` | `gallery_app`, `gallery`, `gallery_tests`; client code in `gallery::client` |
| `examples/tui/workout/` | `workout_app`, `workout`, `workout_tests`; client code in `workout::client` |

## Build and test commands

Three configurations; the first is the master plan's, the other two prove each frontend and each transport is
optional.

```bash
cmake -S . -B build/all -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_BANK_EXAMPLE=ON \
      -DMORPH_BUILD_NET=ON -DMORPH_BUILD_FORMS_QML=ON                                   # once
cmake -S . -B build/ex-tui -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_NET=ON   # once
cmake -S . -B build/ex-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON  # once
cmake --build build/all --target examples_common_app_tests
./build/all/examples/common/app/tests/examples_common_app_tests
```

Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING, "Warnings
are errors"). `build/ex-tui` has no Qt at all and `build/ex-qt` no TUI and no `morph::net`; both keep
`MORPH_BUILD_EXAMPLES` at its default `ON`.

---
### Task 1: `SocketBackend` reports connects and drops

`Connection::ready()` (Task 5) needs to learn, without blocking, when a remote transport comes up and goes down.
`QtWebSocketBackend` implements `IBackend::setConnectHandler`/`setDisconnectHandler`; `SocketBackend` does not
(`docs/spec/core/backend.md`, "Connect/disconnect notifications", says so), so a terminal client could only poll
`waitForConnected`. The fix is smaller than an issue describing it, so it is made here (AGENTS.md, "The bar").

**Files:**
- Modify: `include/morph/net/socket_backend.hpp` — two public overrides after `setReconnectHandler`
  (line ~403); in `Core`: `notify`, two members after `reconnectExec` (line ~922), and edits to `onConnected`
  (~689), `onDisconnected` (~704) and `close` (~732)
- Modify: `tests/net/test_socket_backend.cpp` — append five cases after
  `"SocketBackend: a reconnect handler throwing a non-std::exception leaves the transport usable"` (~1588)
- Modify: `docs/spec/core/backend.md` — "Connect/disconnect notifications" last paragraph (~178) and the
  `### SocketBackend (namespace morph::net)` table (~1910)
- Modify: `CHANGELOG.md` — `## [Unreleased]` → `### Added`

**Interfaces:**
- Consumes: `IBackend::setConnectHandler(const std::function<void()>&)`,
  `IBackend::setDisconnectHandler(const std::function<void()>&)` (`include/morph/core/backend.hpp:522`, `:537`);
  `morph::log::logError(std::format_string<…>, …)` (`core/logger.hpp:256`).
- Produces: `morph::net::SocketBackend::setConnectHandler`, `setDisconnectHandler` — run on the I/O loop; a
  connect handler installed while connected is called once at installation; `close()` clears both and calls
  neither; the disconnect handler runs only when an established connection drops.

- [ ] **Step 1: Write the failing tests**

Append to `tests/net/test_socket_backend.cpp`, after the case named above:

```cpp
TEST_CASE("SocketBackend: the connect handler runs once after the first connect",
          "[net][socket_backend][notifications]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()))};
    std::atomic<int> connects{0};
    // Installed while the dial may still be in progress or may already have completed: either way it runs once.
    backend.setConnectHandler([&] { connects.fetch_add(1); });
    REQUIRE(backend.waitForConnected());
    spinUntil([&] { return connects.load() == 1; });
    CHECK(backend.registerModel("SbEchoModel", nullptr).v != 0U);  // a round trip through the loop
    CHECK(connects.load() == 1);
}

TEST_CASE("SocketBackend: a connect handler installed while connected is called once at installation",
          "[net][socket_backend][notifications]") {
    SharedLoopStack stack;  // connected on return
    std::atomic<int> connects{0};
    stack.backend->setConnectHandler([&] { connects.fetch_add(1); });
    spinUntil([&] { return connects.load() == 1; });
    CHECK(stack.backend->registerModel("SbEchoModel", nullptr).v != 0U);
    CHECK(connects.load() == 1);
}

TEST_CASE("SocketBackend: the disconnect handler runs on a drop, and the connect handler again on reconnect",
          "[net][socket_backend][notifications][disconnect]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketBackend::Config cfg;
    cfg.initialReconnectDelay = std::chrono::milliseconds{50};
    cfg.maxReconnectDelay = std::chrono::milliseconds{200};
    std::atomic<int> connects{0};
    std::atomic<int> drops{0};
    std::uint16_t port = 0;
    std::unique_ptr<morph::net::SocketBackend> backend;
    {
        morph::net::SocketServer first{*server, 0};
        REQUIRE(first.listen());
        port = first.port();
        backend = std::make_unique<morph::net::SocketBackend>(
            "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port)), cfg);
        backend->setConnectHandler([&] { connects.fetch_add(1); });
        backend->setDisconnectHandler([&] { drops.fetch_add(1); });
        REQUIRE(backend->waitForConnected());
        spinUntil([&] { return connects.load() == 1; });
    }
    spinUntil([&] { return drops.load() == 1; });
    CHECK(drops.load() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds{100});  // the OS releases the port
    morph::net::SocketServer second{*server, port};
    REQUIRE(second.listen());
    spinUntil([&] { return connects.load() == 2; });
    CHECK(connects.load() == 2);
    CHECK(drops.load() == 1);
}

TEST_CASE("SocketBackend: a throwing connect handler leaves the transport usable",
          "[net][socket_backend][notifications]") {
    SharedLoopStack stack;
    std::atomic<int> calls{0};
    stack.backend->setConnectHandler([&] {
        calls.fetch_add(1);
        throw std::runtime_error("connect handler blew up");
    });
    spinUntil([&] { return calls.load() == 1; });
    CHECK(calls.load() == 1);
    CHECK(stack.backend->registerModel("SbEchoModel", nullptr).v != 0U);
}

TEST_CASE("SocketBackend: closing the backend calls neither handler", "[net][socket_backend][notifications]") {
    std::atomic<int> drops{0};
    {
        SharedLoopStack stack;
        stack.backend->setDisconnectHandler([&] { drops.fetch_add(1); });
        CHECK(stack.backend->registerModel("SbEchoModel", nullptr).v != 0U);  // the install has run
        stack.backend.reset();
    }
    CHECK(drops.load() == 0);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/all --target morph_net_tests && ./build/all/tests/net/morph_net_tests "[notifications]"`
Expected: FAIL — `connects.load() == 1` is `0 == 1` (the base class stores and ignores the handler), and the
throwing-handler case reports `calls.load() == 1` as `0 == 1`.

- [ ] **Step 3: Implement**

In `include/morph/net/socket_backend.hpp`, after `setReconnectHandler`'s definition, add:

```cpp
    /// @brief Installs the callback run on the I/O loop after every successful connect, the first included.
    ///
    /// Posted: the loop stores it. Installed while the connection is already up, it is also called once at
    /// installation, because on a transport with its own thread the first connect can complete before the
    /// caller installs anything. It runs on the loop's thread, so it must not wait for the loop: post to an
    /// executor of your own for anything more. A throw is logged and the transport goes on. `close()` clears it.
    /// @param handler The callback; `nullptr` clears it.
    void setConnectHandler(const std::function<void()>& handler) override {
        _loop->post([core = _core, handler] {
            core->note("SocketBackend::setConnectHandler");
            if (core->closed) {
                return;
            }
            core->connectHandler = handler;
            if (core->connected.load()) {
                Core::notify(core->connectHandler, "connect");
            }
        });
    }

    /// @brief Installs the callback run on the I/O loop when an established connection drops.
    ///
    /// Posted: the loop stores it. It runs after the pending calls are rejected and before a reconnect is
    /// scheduled, and never for a dial that did not connect or for `close()`, which clears it. Same thread and
    /// throw rules as `setConnectHandler`.
    /// @param handler The callback; `nullptr` clears it.
    void setDisconnectHandler(const std::function<void()>& handler) override {
        _loop->post([core = _core, handler] {
            core->note("SocketBackend::setDisconnectHandler");
            if (!core->closed) {
                core->disconnectHandler = handler;
            }
        });
    }
```

In `struct Core`, add before `onConnected`:

```cpp
        /// Runs a connect or disconnect handler on the loop. A throw is logged here: an exception escaping into
        /// the loop's turn would end the connection's flow.
        static void notify(std::function<void()> const& handler, char const* what) noexcept {
            if (!handler) {
                return;
            }
            try {
                handler();
            } catch (const std::exception& error) {
                ::morph::log::logError("[socket-backend] {} handler threw: {}", what, error.what());
            } catch (...) {
                ::morph::log::logError("[socket-backend] {} handler threw a non-std::exception", what);
            }
        }
```

At the end of `onConnected()`, after the reconnect-handler `if`:

```cpp
            // A copy: the handler may install a replacement, which is posted, but the copy keeps the call
            // independent of the member either way.
            auto const handler = connectHandler;
            notify(handler, "connect");
```

Replace `onDisconnected()` with:

```cpp
        void onDisconnected() {
            // Called after every attempt, a dial that never connected included; only a drop of an
            // established connection is reported.
            bool const wasConnected = connected.exchange(false);
            if (conn) {
                ::morph::net::detail::closeAfterFlush(conn);
                conn.reset();
            }
            cancelAll(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
            if (wasConnected) {
                auto const handler = disconnectHandler;
                notify(handler, "disconnect");
            }
        }
```

In `close()`, after `reconnectHandler = nullptr;`:

```cpp
            connectHandler = nullptr;
            disconnectHandler = nullptr;
```

After the member `::morph::exec::IExecutor* reconnectExec{nullptr};`:

```cpp
        /// Run on the loop after every connect; cleared by `close()`.
        std::function<void()> connectHandler;
        /// Run on the loop when an established connection drops; cleared by `close()`.
        std::function<void()> disconnectHandler;
```

In `docs/spec/core/backend.md`, replace the paragraph beginning "`QtWebSocketBackend` is currently the only
backend that overrides either" with:

```markdown
`QtWebSocketBackend` and `SocketBackend` override both. `QtWebSocketBackend`'s
`connected`/`disconnected` `QWebSocket` signal slots invoke `_connectHandler`/
`_disconnectHandler` (if installed) at the same points they already invoke
`_reconnectHandler`/schedule a reconnect — see that section below.
`SocketBackend` runs both on its I/O loop: the connect handler after every
completed handshake, the disconnect handler when an established connection
drops, after its pending calls are rejected and before a reconnect is
scheduled — never for a dial that did not connect. Because its first connect
runs on the loop's own thread and can complete before a caller installs a
handler, a connect handler installed while the connection is up is also called
once at installation. `close()` clears both and calls neither; a throwing
handler is logged and the transport goes on.
```

In the `### SocketBackend (namespace morph::net)` table, after the `setReconnectHandler` row, add:

```markdown
| `setConnectHandler(handler)` | Posted: the loop stores it and runs it after every completed connect, the first included; installed while connected, it is also called once at installation. On the loop's thread; a throw is logged. `nullptr` clears; `close()` clears. |
| `setDisconnectHandler(handler)` | Posted: the loop stores it and runs it when an established connection drops, after the pending calls are rejected and before a reconnect is scheduled; never for a failed dial or `close()`. `nullptr` clears. |
```

In `CHANGELOG.md`, under `## [Unreleased]` → `### Added`, append:

```markdown
- **`morph::net::SocketBackend` reports connects and drops.** It implements
  `IBackend::setConnectHandler` and `setDisconnectHandler`, run on its I/O loop,
  so a client on the raw-socket transport learns its connection state without
  blocking in `waitForConnected`. A connect handler installed while the
  connection is up is called once at installation; closing the backend calls
  neither.
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/all --target morph_net_tests && ./build/all/tests/net/morph_net_tests "[socket_backend]"`
Expected: PASS, every `[socket_backend]` case (the existing ones prove the `onDisconnected` rewrite changed
nothing else). Mutation check: delete the `if (core->connected.load()) { … }` block in `setConnectHandler`.
Expected FAIL in "a connect handler installed while connected is called once at installation" (`0 == 1`).
Restore. Second mutation: drop the `if (wasConnected)` guard in `onDisconnected`. Expected FAIL in "the
disconnect handler runs on a drop, and the connect handler again on reconnect": the re-dials that fail while no
server listens (50 ms backoff inside the 100 ms gap) each call `onDisconnected`, so the last
`CHECK(drops.load() == 1)` reads `2` or more. Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/net/socket_backend.hpp tests/net/test_socket_backend.cpp docs/spec/core/backend.md CHANGELOG.md
git commit -m "wip(examples-common): SocketBackend reports connects and drops

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `morph_ladder_app_common` and `AppEnvironment`

**Where the target lives, and why.** Three consumers need it: the ladder (`MORPH_BUILD_LADDER`, which is *not*
gated on `MORPH_BUILD_EXAMPLES`), the bank example (`MORPH_BUILD_BANK_EXAMPLE`, added from the root's Demo block,
not the ladder) and `examples/tui` (`MORPH_BUILD_EXAMPLES` plus a frontend). So `examples/common/app/` gets its own
`CMakeLists.txt`, added **once** from the root in a new block placed *after* the Tests block (its tests and
examples/tui's call `include(Catch)` the way `tests/net` does) and *before* the ladder block (so
`morph_add_rung` finds it). `examples/bank`, added earlier from the Demo block, links
`morph::ladder_app_common` by name; CMake resolves target names at generate time, so the order is harmless there,
and bank must not test `if(TARGET …)` for it. The directory is added under Emscripten too (the ladder's WASM
configure builds it), where its tests are skipped.

**Files:**
- Create: `examples/common/app/CMakeLists.txt`, `examples/common/app/app_environment.hpp`,
  `examples/common/app/app_environment.cpp`
- Create: `examples/common/app/tests/CMakeLists.txt`, `examples/common/app/tests/.clang-tidy`,
  `examples/common/app/tests/test_app_environment.cpp`
- Modify: `CMakeLists.txt` (root) — a new block between the end of the `# ── Tests ──` block (`endif()` after
  `add_subdirectory(tests)`) and `# ── Application ladder (optional) ──`

**Interfaces:**
- Consumes: `morph::morph`, `morph_test_main`, `apply_warnings`/`apply_sanitizers`/`apply_coverage`
  (`cmake/compiler_options.cmake`).
- Produces: target `morph_ladder_app_common` / `morph::ladder_app_common` (PUBLIC include dir
  `examples/common`, so headers are `<app/…>` and `<testkit/…>`); `morph::examples::AppEnvironment{server, db,
  user, seed, pollId}` with `static AppEnvironment fromArgs(int, char const* const*)`;
  `morph::examples::detail::applyQuery(AppEnvironment&, std::string_view)`; test target
  `examples_common_app_tests` (ctest label `examples-common`).

- [ ] **Step 1: Write the failing test**

Create `examples/common/app/tests/.clang-tidy`:

```yaml
# Test idiom, not a defect: Catch2's REQUIRE(a == b) expands to a chained comparison. The full reasoning, and
# why the file is per directory, is in examples/common/testkit/.clang-tidy.
Checks: '-bugprone-chained-comparison'
CheckOptions:
  - key:   readability-function-cognitive-complexity.IgnoreMacros
    value: "true"
InheritParentConfig: true
```

Create `examples/common/app/tests/test_app_environment.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <optional>
#include <stdexcept>
#include <string>

using morph::examples::AppEnvironment;

namespace {

template <std::size_t N>
AppEnvironment parse(std::array<char const*, N> const& argv) {
    return AppEnvironment::fromArgs(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE("AppEnvironment: no options leaves every field at its default", "[examples-common][environment]") {
    auto const env = parse(std::array{"app"});
    CHECK_FALSE(env.server.has_value());
    CHECK(env.db.empty());
    CHECK(env.user.empty());
    CHECK_FALSE(env.seed);
    CHECK_FALSE(env.pollId.has_value());
}

TEST_CASE("AppEnvironment: each option reads its value in both spellings", "[examples-common][environment]") {
    auto const spaced = parse(std::array{"app", "--server", "ws://h:1", "--db", "a.db", "--user", "ann", "--poll",
                                         "p1", "--seed"});
    CHECK(spaced.server == std::optional<std::string>{"ws://h:1"});
    CHECK(spaced.db == "a.db");
    CHECK(spaced.user == "ann");
    CHECK(spaced.pollId == std::optional<std::string>{"p1"});
    CHECK(spaced.seed);

    auto const joined = parse(std::array{"app", "--server=ws://h:2", "--db=b.db", "--user=bob", "--poll=p2"});
    CHECK(joined.server == std::optional<std::string>{"ws://h:2"});
    CHECK(joined.db == "b.db");
    CHECK(joined.user == "bob");
    CHECK(joined.pollId == std::optional<std::string>{"p2"});
    CHECK_FALSE(joined.seed);
}

TEST_CASE("AppEnvironment: unknown arguments are left to the frontend", "[examples-common][environment]") {
    auto const env = parse(std::array{"app", "--ui=tui", "-platform", "offscreen", "--db", "x.db"});
    CHECK(env.db == "x.db");
    CHECK_FALSE(env.server.has_value());
}

TEST_CASE("AppEnvironment: a value option without its value is refused", "[examples-common][environment]") {
    CHECK_THROWS_WITH(parse(std::array{"app", "--server"}), Catch::Matchers::ContainsSubstring("--server"));
    CHECK_THROWS_AS(parse(std::array{"app", "--db", "x.db", "--user"}), std::invalid_argument);
}

TEST_CASE("AppEnvironment: a later occurrence wins", "[examples-common][environment]") {
    auto const env = parse(std::array{"app", "--db", "first.db", "--db=second.db"});
    CHECK(env.db == "second.db");
}

TEST_CASE("AppEnvironment: a longer word that starts with an option is not that option",
          "[examples-common][environment]") {
    auto const env = parse(std::array{"app", "--serverx=1", "--dbfile", "y"});
    CHECK_FALSE(env.server.has_value());
    CHECK(env.db.empty());
}

TEST_CASE("applyQuery: reads server and poll, decoded, and ignores the rest", "[examples-common][environment]") {
    AppEnvironment env;
    morph::examples::detail::applyQuery(env, "?poll=abc%2D1&lang=en&server=ws%3A%2F%2Fhost%3A8080&x");
    CHECK(env.pollId == std::optional<std::string>{"abc-1"});
    CHECK(env.server == std::optional<std::string>{"ws://host:8080"});
}

TEST_CASE("applyQuery: a malformed escape is kept, a plus is a space, an empty value is absent",
          "[examples-common][environment]") {
    AppEnvironment env;
    morph::examples::detail::applyQuery(env, "poll=%zz+1&server=");
    CHECK(env.pollId == std::optional<std::string>{"%zz 1"});
    CHECK_FALSE(env.server.has_value());
}
```

Create `examples/common/app/tests/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# examples_common_app_tests: the Qt-free example foundation, tested headless. It links Qt only when the Qt
# transport or the Qt Quick smoke half is built, and its main (morph_test_main) owns no Qt application object,
# which is what lets the Qt Quick smoke construct its own.

add_executable(examples_common_app_tests
    test_app_environment.cpp
)
target_link_libraries(examples_common_app_tests PRIVATE morph::ladder_app_common morph_test_main)
target_compile_features(examples_common_app_tests PRIVATE cxx_std_23)
apply_warnings(examples_common_app_tests)
if(AF_COVERAGE)
    apply_coverage(examples_common_app_tests)
endif()
if(DEFINED AF_SANITIZER)
    apply_sanitizers(examples_common_app_tests ${AF_SANITIZER})
endif()

include(Catch)
catch_discover_tests(examples_common_app_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS examples-common TIMEOUT 120)
```

Create `examples/common/app/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# morph_ladder_app_common: what every example application's composition root and controllers share -- the
# environment a binary reads from its command line, the transport that builds its Bridge, ids, UUIDs and the
# event poller. The ladder, the bank example and examples/tui link it, so the root CMakeLists.txt adds this
# directory when any of them is enabled.
#
# Its interface names no toolkit: an application library links it and still fails to compile on a Qt include.
# The Qt and morph::net transports are separate translation units, compiled in only when their option is on and
# linked PRIVATE, so their include paths never reach a consumer.

if(NOT TARGET morph::morph)
    message(FATAL_ERROR "examples/common/app expects the morph::morph target; configure from the repository root.")
endif()

add_library(morph_ladder_app_common STATIC
    app_environment.cpp
)
add_library(morph::ladder_app_common ALIAS morph_ladder_app_common)
target_include_directories(morph_ladder_app_common PUBLIC "${PROJECT_SOURCE_DIR}/examples/common")
target_link_libraries(morph_ladder_app_common PUBLIC morph::morph)
target_compile_features(morph_ladder_app_common PUBLIC cxx_std_23)
apply_warnings(morph_ladder_app_common)
if(AF_COVERAGE)
    apply_coverage(morph_ladder_app_common)
endif()
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_ladder_app_common ${AF_SANITIZER})
endif()

# Catch2 and a terminal or desktop to run on: neither exists in a WebAssembly configure.
if(MORPH_BUILD_TESTS AND NOT EMSCRIPTEN)
    add_subdirectory(tests)
endif()
```

In the root `CMakeLists.txt`, between the Tests block and `# ── Application ladder (optional) ──`, add:

```cmake
# ── Example applications on an injected frontend ────────────────────────────
# examples/common/app is the toolkit-free foundation every example application
# builds on (see its CMakeLists.txt). The ladder, the bank example, examples/tui
# and the forms demo (examples/forms, added in every native configure with
# examples on) each link it, so it is added once, when any of them is on --
# MORPH_BUILD_LADDER on its own, since the ladder is not gated on
# MORPH_BUILD_EXAMPLES. It comes after the Tests block because its tests call
# include(Catch), and before the ladder block because morph_add_rung links it.
# examples/bank and examples/forms, added from the Demo block above, link it by
# name, which CMake resolves at generate time.
if(MORPH_BUILD_LADDER OR (MORPH_BUILD_EXAMPLES AND (NOT EMSCRIPTEN OR MORPH_BUILD_BANK_EXAMPLE OR MORPH_BUILD_TUI
                                                   OR MORPH_BUILD_QT_QUICK)))
    add_subdirectory(examples/common/app)
endif()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake -S . -B build/all && cmake --build build/all --target examples_common_app_tests`
Expected: FAIL — CMake reports `Cannot find source file: app_environment.cpp`.

- [ ] **Step 3: Implement**

Create `examples/common/app/app_environment.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

/// @file
/// @brief `morph::examples::AppEnvironment`: the deployment choices an example's `main` reads from its command
///        line (and, in a browser, from the page's query string).

namespace morph::examples {

/// @brief What an example application is pointed at: a server or an in-process database, a principal, demo data.
///
/// `main` reads it once and hands it to the application factory; nothing here names a frontend.
struct AppEnvironment {
    /// @brief `--server URL`: run against a remote `RemoteServer`; empty runs the models in-process.
    std::optional<std::string> server;
    /// @brief `--db PATH`: the in-process database; empty lets the application choose its default.
    std::string db;
    /// @brief `--user NAME`: the principal installed as the bridge's default session; empty installs none.
    std::string user;
    /// @brief `--seed`: populate an empty database with demo data.
    bool seed = false;
    /// @brief `--poll ID` natively, `?poll=ID` in a browser: the poll a polls client opens first.
    std::optional<std::string> pollId;

    /// @brief Reads the options above from @p argv, and under WebAssembly also from the page's query string.
    ///
    /// Each value option is accepted as `--name value` or `--name=value`; a later occurrence wins. Every other
    /// argument belongs to the frontend (`--ui=`, Qt's own options) and is ignored here.
    /// @param argc The argument count `main` received.
    /// @param argv The arguments `main` received; `argv[0]` is the program and is skipped.
    /// @return The environment.
    /// @throws std::invalid_argument when a value option is the last argument.
    [[nodiscard]] static AppEnvironment fromArgs(int argc, char const* const* argv);
};

namespace detail {

/// @brief Applies a URL query string's `server` and `poll` parameters to @p env.
///
/// Other parameters are ignored, `+` decodes to a space and `%XX` to its byte (a malformed escape is kept as
/// written), and a parameter with an empty value leaves its field as it was.
/// @param env The environment to update.
/// @param query The query, with or without its leading `?`.
void applyQuery(AppEnvironment& env, std::string_view query);

}  // namespace detail

}  // namespace morph::examples
```

Create `examples/common/app/app_environment.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "app/app_environment.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>

#include <cstdlib>
#endif

namespace morph::examples {

namespace {

/// One option that takes a value, and where the value goes.
struct ValueOption {
    std::string_view name;
    void (*assign)(AppEnvironment&, std::string);
};

constexpr std::array<ValueOption, 4> kValueOptions{{
    {"--server", [](AppEnvironment& env, std::string value) { env.server = std::move(value); }},
    {"--db", [](AppEnvironment& env, std::string value) { env.db = std::move(value); }},
    {"--user", [](AppEnvironment& env, std::string value) { env.user = std::move(value); }},
    {"--poll", [](AppEnvironment& env, std::string value) { env.pollId = std::move(value); }},
}};

/// The value option @p arg names, and its inline `=value` when it carries one.
struct Match {
    ValueOption const* option = nullptr;
    std::optional<std::string_view> inlineValue;
};

Match match(std::string_view arg) {
    for (auto const& option : kValueOptions) {
        if (arg == option.name) {
            return Match{.option = &option, .inlineValue = std::nullopt};
        }
        if (arg.size() > option.name.size() && arg.starts_with(option.name) && arg.at(option.name.size()) == '=') {
            return Match{.option = &option, .inlineValue = arg.substr(option.name.size() + 1)};
        }
    }
    return Match{};
}

std::optional<unsigned> hexDigit(char digit) {
    if (digit >= '0' && digit <= '9') {
        return static_cast<unsigned>(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f') {
        return static_cast<unsigned>(digit - 'a') + 10U;
    }
    if (digit >= 'A' && digit <= 'F') {
        return static_cast<unsigned>(digit - 'A') + 10U;
    }
    return std::nullopt;
}

std::string decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char const current = text.at(i);
        if (current == '+') {
            out.push_back(' ');
            continue;
        }
        if (current == '%' && i + 2 < text.size()) {
            auto const high = hexDigit(text.at(i + 1));
            auto const low = hexDigit(text.at(i + 2));
            if (high && low) {
                out.push_back(static_cast<char>((*high << 4U) | *low));
                i += 2;
                continue;
            }
        }
        out.push_back(current);
    }
    return out;
}

#if defined(__EMSCRIPTEN__)
// Returns a malloc'd UTF-8 copy of `window.location.search`, freed by the caller. EM_JS bodies reach the
// runtime's string helpers without -sEXPORTED_RUNTIME_METHODS, and need no Embind.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): EM_JS defines a function through a macro.
EM_JS(char*, morphExamplesPageQuery, (), {
    var query = window.location.search;
    var length = lengthBytesUTF8(query) + 1;
    var ptr = _malloc(length);
    stringToUTF8(query, ptr, length);
    return ptr;
});

std::string pageQuery() {
    char* raw = morphExamplesPageQuery();
    std::string query{raw};
    std::free(raw);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): EM_JS hands out malloc'd memory.
    return query;
}
#endif

}  // namespace

AppEnvironment AppEnvironment::fromArgs(int argc, char const* const* argv) {
    AppEnvironment env;
    auto const args = std::span{argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0}};
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string_view const arg = args[i] != nullptr ? std::string_view{args[i]} : std::string_view{};
        if (arg == "--seed") {
            env.seed = true;
            continue;
        }
        auto const found = match(arg);
        if (found.option == nullptr) {
            continue;
        }
        if (found.inlineValue) {
            found.option->assign(env, std::string{*found.inlineValue});
            continue;
        }
        if (i + 1 >= args.size() || args[i + 1] == nullptr) {
            throw std::invalid_argument{std::string{found.option->name} + " needs a value"};
        }
        ++i;
        found.option->assign(env, std::string{args[i]});
    }
#if defined(__EMSCRIPTEN__)
    detail::applyQuery(env, pageQuery());
#endif
    return env;
}

void detail::applyQuery(AppEnvironment& env, std::string_view query) {
    if (query.starts_with('?')) {
        query.remove_prefix(1);
    }
    while (!query.empty()) {
        auto const amp = query.find('&');
        auto const pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        auto const equals = pair.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        auto const key = decode(pair.substr(0, equals));
        auto value = decode(pair.substr(equals + 1));
        if (value.empty()) {
            continue;
        }
        if (key == "server") {
            env.server = std::move(value);
        } else if (key == "poll") {
            env.pollId = std::move(value);
        }
    }
}

}  // namespace morph::examples
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[environment]"
```

Expected: PASS, 8 test cases. Also `cmake --build build/ex-tui --target examples_common_app_tests` and the same
for `build/ex-qt` build (the block's condition holds there through `MORPH_BUILD_TUI` / `MORPH_BUILD_QT_QUICK`).
Mutation check: in `match()`, replace `arg.at(option.name.size()) == '='` with `true`. Expected FAIL in "a longer
word that starts with an option is not that option" (`--serverx=1` sets `server`). Restore.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt examples/common/app
git commit -m "wip(examples-common): morph_ladder_app_common and AppEnvironment

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `newUuid()` and the id helpers

`QUuid` idempotency keys (kanban, ledger) and `id_qml.hpp`'s `QString` forms get Qt-free replacements.
`id_qml.hpp` stays until Part 10; nothing switches to these helpers in this part.

**Files:**
- Create: `examples/common/app/uuid.hpp`, `examples/common/app/uuid.cpp`, `examples/common/app/ids.hpp`
- Modify: `examples/common/app/CMakeLists.txt` — add `uuid.cpp` to `morph_ladder_app_common`'s sources, after
  `app_environment.cpp`
- Modify: `examples/common/app/tests/CMakeLists.txt` — add `test_uuid.cpp` and `test_ids.cpp` after
  `test_app_environment.cpp`
- Test: `examples/common/app/tests/test_uuid.cpp`, `examples/common/app/tests/test_ids.cpp`

**Interfaces:**
- Consumes: `morph::ui::Key` (`include/morph/ui/view.hpp`, Part 2).
- Produces: `morph::examples::newUuid() -> std::string`; in `ids.hpp`: `kNoId` (`std::int64_t{-1}`),
  `idNumber(Id const&) -> std::int64_t`, `idText(Id const&) -> std::string`,
  `idFromText<Id>(std::string_view) -> Id`, `idKey(Id const&) -> ui::Key`, `idFromKey<Id>(ui::Key const&) -> Id`.

- [ ] **Step 1: Write the failing tests**

Create `examples/common/app/tests/test_uuid.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/uuid.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <regex>
#include <set>
#include <string>

TEST_CASE("newUuid: an RFC 4122 version-4 UUID in lower case", "[examples-common][uuid]") {
    std::regex const shape{"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
    for (int i = 0; i < 100; ++i) {
        auto const uuid = morph::examples::newUuid();
        INFO(uuid);
        CHECK(std::regex_match(uuid, shape));
    }
}

TEST_CASE("newUuid: a thousand in a row are distinct", "[examples-common][uuid]") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(morph::examples::newUuid());
    }
    CHECK(seen.size() == std::size_t{1000});
}
```

Create `examples/common/app/tests/test_ids.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/ids.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>

using morph::examples::idFromKey;
using morph::examples::idFromText;
using morph::examples::idKey;
using morph::examples::idNumber;
using morph::examples::idText;
using morph::examples::kNoId;

namespace {

// The two id shapes the examples have: payload in an optional, and a plain integer whose 0 means "none".
struct OptionalId {
    std::optional<std::int64_t> value;
    OptionalId() = default;
    explicit OptionalId(std::int64_t payload) : value{payload} {}
    [[nodiscard]] bool hasValue() const { return value.has_value(); }
    [[nodiscard]] std::int64_t operator*() const { return *value; }  // NOLINT(bugprone-unchecked-optional-access)
    bool operator==(OptionalId const&) const = default;
};

struct ZeroSentinel {
    std::int64_t value = 0;
    [[nodiscard]] bool hasValue() const { return value != 0; }
    [[nodiscard]] std::int64_t operator*() const { return value; }
    bool operator==(ZeroSentinel const&) const = default;
};

}  // namespace

TEST_CASE("ids: unset, zero and an ordinary id stay apart", "[examples-common][ids]") {
    CHECK(idNumber(OptionalId{}) == kNoId);
    CHECK(idNumber(OptionalId{0}) == 0);
    CHECK(idNumber(OptionalId{7}) == 7);
    CHECK(idText(OptionalId{}).empty());
    CHECK(idText(OptionalId{0}) == "0");
    CHECK(idText(OptionalId{7}) == "7");
}

TEST_CASE("ids: a zero-sentinel id's zero is its empty state", "[examples-common][ids]") {
    CHECK(idNumber(ZeroSentinel{}) == kNoId);
    CHECK(idText(ZeroSentinel{}).empty());
    CHECK(idText(ZeroSentinel{9}) == "9");
}

TEST_CASE("ids: text round-trips, and anything but a whole integer is the empty id", "[examples-common][ids]") {
    CHECK(idFromText<OptionalId>("42") == OptionalId{42});
    CHECK(idFromText<OptionalId>("0") == OptionalId{0});
    CHECK(idFromText<OptionalId>("") == OptionalId{});
    CHECK(idFromText<OptionalId>("4x") == OptionalId{});
    CHECK(idFromText<OptionalId>(" 4") == OptionalId{});
    CHECK(idFromText<OptionalId>("9223372036854775807") == OptionalId{INT64_MAX});
    CHECK(idFromText<OptionalId>("9223372036854775808") == OptionalId{});
}

TEST_CASE("ids: a view key carries the id as an integer, never a double", "[examples-common][ids]") {
    std::int64_t const big = (std::int64_t{1} << 53) + 1;
    morph::ui::Key const key = idKey(OptionalId{big});
    REQUIRE(std::holds_alternative<std::int64_t>(key));
    CHECK(std::get<std::int64_t>(key) == big);
    CHECK(idFromKey<OptionalId>(key) == OptionalId{big});
    CHECK(idFromKey<OptionalId>(idKey(OptionalId{})) == OptionalId{});
    CHECK(idFromKey<OptionalId>(morph::ui::Key{std::string{"17"}}) == OptionalId{17});
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/all --target examples_common_app_tests`
Expected: FAIL — `'app/uuid.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/common/app/uuid.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// @brief `morph::examples::newUuid`: fresh idempotency keys without a toolkit.

namespace morph::examples {

/// @brief A random RFC 4122 version-4 UUID, `xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx` in lower case.
///
/// Each thread draws from its own generator, seeded once from `std::random_device`; the keys are unique, not
/// secret.
/// @return The UUID text.
[[nodiscard]] std::string newUuid();

}  // namespace morph::examples
```

Create `examples/common/app/uuid.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "app/uuid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace morph::examples {

std::string newUuid() {
    thread_local std::mt19937_64 generator{[] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        return std::mt19937_64{seed};
    }()};
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 8) {
        auto word = generator();
        for (std::size_t j = 0; j < 8; ++j) {
            bytes.at(i + j) = static_cast<std::uint8_t>(word & 0xFFU);
            word >>= 8U;
        }
    }
    bytes.at(6) = static_cast<std::uint8_t>((bytes.at(6) & 0x0FU) | 0x40U);  // version 4
    bytes.at(8) = static_cast<std::uint8_t>((bytes.at(8) & 0x3FU) | 0x80U);  // variant 10
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHex.at(bytes.at(i) >> 4U));
        out.push_back(kHex.at(bytes.at(i) & 0x0FU));
    }
    return out;
}

}  // namespace morph::examples
```

Create `examples/common/app/ids.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <charconv>
#include <concepts>
#include <cstdint>
#include <morph/ui/view.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

/// @file
/// @brief Strong id ↔ text and ↔ `ui::Key`, once, for every example.
///
/// An id here is anything with `hasValue()`, `operator*()` yielding the integer, and `Id{std::int64_t}`. That
/// covers both shapes the examples have: an optional payload (an unset id and an id holding 0 differ) and a
/// plain integer whose own 0 means "none" (`hasValue()` is `value != 0`). The guarantee is stated in
/// `hasValue()`'s terms, the only one true of both: whatever the id calls empty maps to the empty
/// representation, and everything it calls engaged — 0 included — maps to its own payload.

namespace morph::examples {

/// @brief What an id looks like to these helpers.
/// @tparam Id The strong id type.
template <class Id>
concept StrongId = requires(Id const& strongId) {
    { strongId.hasValue() } -> std::convertible_to<bool>;
    { *strongId } -> std::convertible_to<std::int64_t>;
};

/// @brief The integer standing in for an empty id: ids are database keys starting at 1, so -1 names no row,
///        while 0 is a real value for an optional-backed id.
inline constexpr std::int64_t kNoId = -1;

/// @brief The id as an integer.
/// @tparam Id The strong id type.
/// @param strongId The id.
/// @return Its payload, or `kNoId` when it is empty.
template <StrongId Id>
[[nodiscard]] std::int64_t idNumber(Id const& strongId) {
    return strongId.hasValue() ? static_cast<std::int64_t>(*strongId) : kNoId;
}

/// @brief The id as decimal text.
/// @tparam Id The strong id type.
/// @param strongId The id.
/// @return Its payload in decimal, or an empty string when it is empty.
template <StrongId Id>
[[nodiscard]] std::string idText(Id const& strongId) {
    return strongId.hasValue() ? std::to_string(static_cast<std::int64_t>(*strongId)) : std::string{};
}

/// @brief Parses decimal text into an id.
/// @tparam Id The strong id type.
/// @param text The text; the whole of it must be one integer in `std::int64_t`'s range.
/// @return The id, or the empty id for anything else.
template <StrongId Id>
[[nodiscard]] Id idFromText(std::string_view text) {
    std::int64_t value = 0;
    auto const* const end = text.data() + text.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto const [ptr, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc{} || ptr != end) {
        return Id{};
    }
    return Id{value};
}

/// @brief The id as a view key: an integer, so an id above 2^53 survives every frontend.
/// @tparam Id The strong id type.
/// @param strongId The id.
/// @return `ui::Key{idNumber(strongId)}`.
template <StrongId Id>
[[nodiscard]] ui::Key idKey(Id const& strongId) {
    return ui::Key{idNumber(strongId)};
}

/// @brief The id a view key names.
/// @tparam Id The strong id type.
/// @param key An integer key (`kNoId` is the empty id) or a decimal string key.
/// @return The id, or the empty id.
template <StrongId Id>
[[nodiscard]] Id idFromKey(ui::Key const& key) {
    if (auto const* number = std::get_if<std::int64_t>(&key)) {
        return *number == kNoId ? Id{} : Id{*number};
    }
    return idFromText<Id>(std::get<std::string>(key));
}

}  // namespace morph::examples
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[uuid],[ids]"
```

Expected: PASS, 6 test cases. Mutation check: delete the `bytes.at(8) = …` variant line. Expected FAIL in "an
RFC 4122 version-4 UUID in lower case" (a fourth group not starting with 8, 9, a or b, within 100 draws).
Restore. Second: drop `ptr != end` from `idFromText`. Expected FAIL in "text round-trips" (`"4x"` parses to 4).
Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/common/app
git commit -m "wip(examples-common): newUuid and the id helpers

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: Test waits and `FakeAppContext`

`testkit/wait.hpp` is the Qt-free counterpart of `pump.hpp`: it pumps the owner executor a controller test runs
on, scaled by the same `MORPH_LADDER_DEADLINE_MS` knob (`testkit/deadline.hpp`). `pump.hpp` stays for the rig's
Socket mode, whose server is Qt. `FakeAppContext` is the `ui::AppContext` every controller and view test in Parts
7–10 builds an application on: a `MainThreadExecutor` owner, a `Runtime`, a `ManualScheduler`, a recorded quit.
Both are header-only and reached through `morph_ladder_app_common`'s include path (`examples/common`).

**Files:**
- Create: `examples/common/testkit/wait.hpp`, `examples/common/testkit/fake_app_context.hpp`
- Modify: `examples/common/app/tests/CMakeLists.txt` — add `test_wait.cpp` after `test_ids.cpp`
- Test: `examples/common/app/tests/test_wait.cpp`

**Interfaces:**
- Consumes: `exec::MainThreadExecutor::{runOnce, runFor}` (`core/executor.hpp:280`, `:307`);
  `ladder::testkit::StepExecutor::runOne` (`examples/common/testkit/step_executor.hpp`);
  `ladder::testkit::detail::scaledDeadlineMs` (`testkit/deadline.hpp`); `async::Completion<T>::makeSettleable`;
  `reactive::Runtime`, `reactive::testing::ManualScheduler` (Part 1); `ui::AppContext`, `ui::Scheduler` (Part 2);
  `exec::IoLoop`.
- Produces: `morph::examples::testing::pumpUntil(exec::MainThreadExecutor&, Pred, std::chrono::milliseconds = 2000ms)
  -> bool` (the contract's signature), plus an overload on `ladder::testkit::StepExecutor&`;
  `awaitOn(exec::MainThreadExecutor&, async::Completion<T>, std::chrono::milliseconds = 2000ms) -> T`;
  `FakeAppContext(std::string frontendName = "test", exec::IoLoop* ioLoop = nullptr)` with `owner()`,
  `manualScheduler()`, `quitCode()`.

- [ ] **Step 1: Write the failing test**

Create `examples/common/app/tests/test_wait.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <optional>
#include <stdexcept>
#include <testkit/fake_app_context.hpp>
#include <testkit/step_executor.hpp>
#include <testkit/wait.hpp>
#include <thread>

using morph::examples::testing::awaitOn;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;
using namespace std::chrono_literals;

TEST_CASE("pumpUntil: runs posted work until the predicate holds", "[examples-common][wait]") {
    morph::exec::MainThreadExecutor owner;
    bool done = false;
    owner.post([&] { owner.post([&] { done = true; }); });
    CHECK(pumpUntil(owner, [&] { return done; }));
}

TEST_CASE("pumpUntil: sees work posted from another thread", "[examples-common][wait]") {
    morph::exec::MainThreadExecutor owner;
    bool done = false;
    std::thread poster{[&] { owner.post([&] { done = true; }); }};
    CHECK(pumpUntil(owner, [&] { return done; }));
    poster.join();
}

TEST_CASE("pumpUntil: returns false when the budget runs out", "[examples-common][wait]") {
    morph::exec::MainThreadExecutor owner;
    CHECK_FALSE(pumpUntil(owner, [] { return false; }, 20ms));
}

TEST_CASE("pumpUntil: drives a StepExecutor the same way", "[examples-common][wait]") {
    morph::ladder::testkit::StepExecutor owner;
    std::atomic<bool> done{false};
    std::thread poster{[&] { owner.post([&] { done = true; }); }};
    CHECK(pumpUntil(owner, [&] { return done.load(); }));
    poster.join();
    CHECK_FALSE(pumpUntil(owner, [] { return false; }, 20ms));
}

TEST_CASE("awaitOn: returns the value, rethrows the failure, and gives up at the budget",
          "[examples-common][wait]") {
    morph::exec::MainThreadExecutor owner;
    {
        auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&owner);
        std::thread settler{[&promise] { promise.resolve(7); }};
        CHECK(awaitOn(owner, std::move(completion)) == 7);
        settler.join();
    }
    {
        auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&owner);
        promise.reject(std::make_exception_ptr(std::runtime_error{"nope"}));
        CHECK_THROWS_WITH(awaitOn(owner, std::move(completion)), "nope");
    }
    {
        auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&owner);
        CHECK_THROWS_AS(awaitOn(owner, std::move(completion), 20ms), std::runtime_error);
    }
}

TEST_CASE("FakeAppContext: a headless context with a manual clock and a recorded quit", "[examples-common][wait]") {
    FakeAppContext ctx;
    CHECK(ctx.frontendName() == "test");
    CHECK(ctx.ioLoop() == nullptr);
    CHECK(&ctx.executor() == &ctx.owner());
    CHECK(&ctx.runtime().owner() == &ctx.owner());

    int fired = 0;
    auto const timer = ctx.scheduler().after(10ms, [&] { ++fired; });
    ctx.manualScheduler().advance(9ms);
    CHECK(fired == 0);
    ctx.manualScheduler().advance(1ms);
    CHECK(fired == 1);

    CHECK_FALSE(ctx.quitCode().has_value());
    ctx.quit(3);
    CHECK(ctx.quitCode() == std::optional<int>{3});
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target examples_common_app_tests`
Expected: FAIL — `'testkit/fake_app_context.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/common/testkit/wait.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <concepts>
#include <exception>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "testkit/deadline.hpp"
#include "testkit/step_executor.hpp"

/// @file
/// @brief Waits for a controller test, over the owner executor the code under test delivers on — no Qt, no
///        sleeps. Every budget is scaled by `MORPH_LADDER_DEADLINE_MS` (`testkit/deadline.hpp`), the knob every
///        ladder wait obeys.

namespace morph::examples::testing {

namespace detail {

/// @brief The moment a wait with @p budget gives up.
/// @param budget The unscaled budget.
/// @return Now plus the scaled budget.
[[nodiscard]] inline std::chrono::steady_clock::time_point deadlineAfter(std::chrono::milliseconds budget) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds{::morph::ladder::testkit::detail::scaledDeadlineMs(budget.count())};
}

}  // namespace detail

/// @brief Runs @p owner's tasks until @p done holds or the budget runs out.
///
/// Runs whatever is queued, then blocks on the executor for at most a millisecond at a time, so work posted from
/// another thread (a worker pool, an I/O loop) is picked up as soon as it arrives.
/// @tparam Pred A predicate taking no arguments.
/// @param owner The executor the code under test delivers on; this thread must be the one that runs it.
/// @param done The condition to wait for.
/// @param budget How long to wait before giving up, before scaling.
/// @return Whether @p done holds.
template <std::predicate<> Pred>
[[nodiscard]] bool pumpUntil(exec::MainThreadExecutor& owner, Pred done,
                             std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
    auto const deadline = detail::deadlineAfter(budget);
    while (!done()) {
        if (owner.runOnce()) {
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return done();
        }
        owner.runFor(std::chrono::milliseconds{1});
    }
    return true;
}

/// @brief The same wait over a `StepExecutor`, which cannot block: between empty polls the thread yields.
/// @tparam Pred A predicate taking no arguments.
/// @param owner The executor the code under test delivers on.
/// @param done The condition to wait for.
/// @param budget How long to wait before giving up, before scaling.
/// @return Whether @p done holds.
template <std::predicate<> Pred>
[[nodiscard]] bool pumpUntil(::morph::ladder::testkit::StepExecutor& owner, Pred done,
                             std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
    auto const deadline = detail::deadlineAfter(budget);
    while (!done()) {
        if (owner.runOne()) {
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return done();
        }
        std::this_thread::yield();
    }
    return true;
}

/// @brief Waits for @p completion, pumping @p owner, and returns its value.
///
/// The outcome lives in shared state the handlers own, so a completion that settles after a timeout writes into
/// memory nobody reads rather than into this unwound frame.
/// @tparam T The value type.
/// @param owner The completion's executor; this thread must be the one that runs it.
/// @param completion The completion to wait for.
/// @param budget How long to wait before giving up, before scaling.
/// @return The value.
/// @throws The completion's own exception when it fails, or `std::runtime_error` when the budget runs out.
template <class T>
T awaitOn(exec::MainThreadExecutor& owner, async::Completion<T> completion,
          std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
    struct Outcome {
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto const outcome = std::make_shared<Outcome>();
    completion.then([outcome](T const& value) { outcome->value = value; })
        .onError([outcome](std::exception_ptr error) { outcome->error = std::move(error); });
    if (!pumpUntil(owner, [&] { return outcome->value.has_value() || outcome->error != nullptr; }, budget)) {
        throw std::runtime_error{"awaitOn: the budget ran out before the completion settled"};
    }
    if (outcome->error != nullptr) {
        std::rethrow_exception(outcome->error);
    }
    return std::move(*outcome->value);
}

}  // namespace morph::examples::testing
```

Create `examples/common/testkit/fake_app_context.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/frontend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief `morph::examples::testing::FakeAppContext`: the context a frontend hands an application, without a
///        frontend — for controller and view tests.

namespace morph::examples::testing {

/// @brief A headless `ui::AppContext`: a `MainThreadExecutor` the test thread pumps, a runtime owned by it, a
///        `ManualScheduler` whose time moves only when the test says, and a quit that is recorded, not obeyed.
///
/// Destroy every application built on it first: it owns the runtime and the scheduler they hold.
class FakeAppContext final : public ui::AppContext {
public:
    /// @param frontendName What `frontendName()` reports — `"tui"` or `"qt"` selects that remote transport.
    /// @param ioLoop What `ioLoop()` reports. Borrowed; null for none.
    explicit FakeAppContext(std::string frontendName = "test", exec::IoLoop* ioLoop = nullptr)
        : _name{std::move(frontendName)}, _ioLoop{ioLoop} {}

    ~FakeAppContext() override = default;
    FakeAppContext(FakeAppContext const&) = delete;
    FakeAppContext& operator=(FakeAppContext const&) = delete;
    FakeAppContext(FakeAppContext&&) = delete;
    FakeAppContext& operator=(FakeAppContext&&) = delete;

    reactive::Runtime& runtime() override { return _runtime; }
    exec::IExecutor& executor() override { return _owner; }
    ui::Scheduler& scheduler() override { return _scheduler; }
    exec::IoLoop* ioLoop() override { return _ioLoop; }
    void quit(int exitCode) override { _quitCode = exitCode; }
    [[nodiscard]] std::string_view frontendName() const override { return _name; }

    /// @brief The owner executor, to pump.
    /// @return The executor `executor()` returns.
    [[nodiscard]] exec::MainThreadExecutor& owner() noexcept { return _owner; }

    /// @brief The scheduler, to advance.
    /// @return The scheduler `scheduler()` returns.
    [[nodiscard]] reactive::testing::ManualScheduler& manualScheduler() noexcept { return _scheduler; }

    /// @brief The code the application last asked to quit with.
    /// @return The exit code, or nothing when `quit` was not called.
    [[nodiscard]] std::optional<int> quitCode() const noexcept { return _quitCode; }

private:
    std::string _name;
    exec::IoLoop* _ioLoop;
    exec::MainThreadExecutor _owner;
    reactive::Runtime _runtime{_owner};
    reactive::testing::ManualScheduler _scheduler;
    std::optional<int> _quitCode;
};

}  // namespace morph::examples::testing
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[wait]"
```

Expected: PASS, 6 test cases. Mutation check: in the `MainThreadExecutor` overload, delete the
`if (owner.runOnce()) { continue; }` statement and replace `owner.runFor(...)` with
`std::this_thread::yield()`. Expected FAIL in "runs posted work until the predicate holds" (nothing runs the
queue; `pumpUntil` returns false). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/common/testkit/wait.hpp examples/common/testkit/fake_app_context.hpp examples/common/app/tests
git commit -m "wip(examples-common): pumpUntil, awaitOn and FakeAppContext

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: `Connection` and `connect()` — the local transport and readiness

`connect` builds the `Bridge` inside an application factory, on `ctx.executor()` (spec 4 §3). This task ships
the local path, the error for a frontend with no remote transport, and `Connection::ready()` — a tracked
`Signal<bool>` a controller's query keys read, so a remote client issues nothing before its transport is up (a
keyed attach issued before `QtWebSocketBackend` connects is rejected outright, `docs/spec/core/backend.md`, "The
structural registration surface"). Task 6 adds the two remote transports.

**Files:**
- Create: `examples/common/app/transport.hpp`, `examples/common/app/transport.cpp`
- Modify: `examples/common/app/CMakeLists.txt` — add `transport.cpp` after `uuid.cpp`; after
  `target_compile_features(morph_ladder_app_common …)` add
  `target_compile_definitions(morph_ladder_app_common PUBLIC MORPH_EXAMPLES_TRANSPORT_QT=0 MORPH_EXAMPLES_TRANSPORT_NET=0)`
  (Task 6 computes them)
- Modify: `examples/common/app/tests/CMakeLists.txt` — add `test_transport.cpp` after `test_wait.cpp`
- Test: `examples/common/app/tests/test_transport.cpp`

**Interfaces:**
- Consumes: `bridge::Bridge(std::unique_ptr<backend::detail::IBackend>, exec::IExecutor&)` (`core/bridge.hpp:715`),
  `Bridge::setDefaultSession`, `Bridge::defaultSession()`; `backend::LocalBackend(exec::IExecutor&)`
  (`core/backend.hpp:1041`); `IBackend::setConnectHandler`/`setDisconnectHandler`; `exec::ThreadPoolExecutor`;
  `reactive::Signal<bool>`; `ui::AppContext`; `testing::FakeAppContext`, `testing::awaitOn`,
  `testing::pumpUntil` (Task 4).
- Produces (the contract's Part 6 names, plus two additions marked †): `TransportError`, `LocalSetup{setupDatabase,
  workers}`, `Link{Local, Remote}` †, `Connection(ui::AppContext&, Link, std::unique_ptr<backend::detail::IBackend>,
  std::unique_ptr<exec::ThreadPoolExecutor> = nullptr)` with `bridge()`, `callbacks()`, `ready()` †
  (`reactive::Signal<bool> const&`), `link()`; `connect(ui::AppContext&, AppEnvironment const&, LocalSetup)`.

- [ ] **Step 1: Write the failing test**

Create `examples/common/app/tests/test_transport.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <app/transport.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <string>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>

struct CommonPing {
    int value = 0;
};

struct CommonPingModel {
    int execute(CommonPing ping) { return ping.value + 1; }
};

BRIDGE_REGISTER_MODEL(CommonPingModel, "CommonPingModel")
BRIDGE_REGISTER_ACTION(CommonPingModel, CommonPing, "CommonPing")

namespace {

using morph::examples::AppEnvironment;
using morph::examples::Connection;
using morph::examples::Link;
using morph::examples::LocalSetup;
using morph::examples::TransportError;
using morph::examples::testing::awaitOn;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;

// What a remote transport hands its connect and disconnect hooks, kept after the Connection clears them, so a
// test can fire a notification that raced the teardown.
struct Notifications {
    std::function<void()> connect;
    std::function<void()> disconnect;
    int clears = 0;
};

// A LocalBackend that reports connection state the way a remote transport does: through the two hooks.
class NotifyingBackend final : public morph::backend::LocalBackend {
public:
    NotifyingBackend(morph::exec::IExecutor& pool, Notifications& seen) : LocalBackend{pool}, _seen{&seen} {}

    void setConnectHandler(std::function<void()> const& handler) override {
        if (handler) {
            _seen->connect = handler;
        } else {
            ++_seen->clears;
        }
    }
    void setDisconnectHandler(std::function<void()> const& handler) override {
        if (handler) {
            _seen->disconnect = handler;
        } else {
            ++_seen->clears;
        }
    }

private:
    Notifications* _seen;
};

std::unique_ptr<Connection> notifyingRemote(FakeAppContext& ctx, Notifications& seen) {
    auto pool = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
    auto backend = std::make_unique<NotifyingBackend>(*pool, seen);
    return std::make_unique<Connection>(ctx, Link::Remote, std::move(backend), std::move(pool));
}

}  // namespace

TEST_CASE("connect: without --server the models run in-process, ready at once", "[examples-common][transport]") {
    FakeAppContext ctx;
    auto const connection = morph::examples::connect(ctx, AppEnvironment{}, LocalSetup{});
    CHECK(connection->link() == Link::Local);
    CHECK(connection->ready().peek());
    CHECK(&connection->callbacks() == &ctx.executor());
    morph::bridge::BridgeHandler<CommonPingModel> handler{connection->bridge(), &connection->callbacks()};
    CHECK(awaitOn(ctx.owner(), handler.execute(CommonPing{41})) == 42);
}

TEST_CASE("connect: the database named by --db is set up before the bridge exists", "[examples-common][transport]") {
    FakeAppContext ctx;
    AppEnvironment env;
    env.db = "lab.db";
    std::string setUp;
    auto const connection = morph::examples::connect(
        ctx, env, LocalSetup{.setupDatabase = [&](std::string const& database) { setUp = database; }, .workers = 2});
    CHECK(setUp == "lab.db");
}

TEST_CASE("connect: --user installs the default session, and no --user installs none",
          "[examples-common][transport]") {
    FakeAppContext ctx;
    AppEnvironment env;
    env.user = "ann";
    CHECK(morph::examples::connect(ctx, env, LocalSetup{})->bridge().defaultSession().principal == "ann");
    CHECK(morph::examples::connect(ctx, AppEnvironment{}, LocalSetup{})->bridge().defaultSession().principal.empty());
}

TEST_CASE("connect: --server on a frontend with no remote transport is a TransportError naming it",
          "[examples-common][transport]") {
    FakeAppContext ctx{"test"};
    AppEnvironment env;
    env.server = "ws://127.0.0.1:1";
    CHECK_THROWS_MATCHES(morph::examples::connect(ctx, env, LocalSetup{}), TransportError,
                         Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("'test'")));
}

TEST_CASE("Connection: a remote link is ready only between a connect and a drop", "[examples-common][transport]") {
    FakeAppContext ctx;
    Notifications seen;
    auto const connection = notifyingRemote(ctx, seen);
    CHECK_FALSE(connection->ready().peek());
    REQUIRE(seen.connect);
    REQUIRE(seen.disconnect);

    seen.connect();  // the transport's thread; the Connection posts to the owner
    CHECK_FALSE(connection->ready().peek());
    CHECK(pumpUntil(ctx.owner(), [&] { return connection->ready().peek(); }));

    seen.disconnect();
    CHECK(pumpUntil(ctx.owner(), [&] { return !connection->ready().peek(); }));
}

TEST_CASE("Connection: a connect notification after the connection is gone touches nothing",
          "[examples-common][transport]") {
    FakeAppContext ctx;
    Notifications seen;
    auto connection = notifyingRemote(ctx, seen);
    connection.reset();
    CHECK(seen.clears == 2);  // both hooks were cleared on the way out

    // A notification already running on the transport's thread when the hooks were cleared.
    seen.connect();
    seen.disconnect();
    ctx.owner().drain();  // the posted updates find the connection gone (ASan observes)
    SUCCEED();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target examples_common_app_tests`
Expected: FAIL — `'app/transport.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/common/app/transport.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/frontend.hpp>
#include <stdexcept>
#include <string>

#include "app/app_environment.hpp"

/// @file
/// @brief `morph::examples::connect`: the `Bridge` an example application talks to, in-process or remote,
///        chosen from its `AppEnvironment` and the frontend it runs on.

namespace morph::examples {

/// @brief The environment asks for a transport this build or this frontend cannot provide.
class TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief How to run the models in-process.
struct LocalSetup {
    /// @brief Creates or migrates the database `--db` names (empty: the application's default); may be empty.
    std::function<void(std::string const& database)> setupDatabase;
    /// @brief Worker threads for the models; under single-threaded WebAssembly the owner executor runs them.
    std::size_t workers = 4;
};

/// @brief Where a connection's models run.
enum class Link : std::uint8_t {
    Local,   ///< In-process, on a worker pool.
    Remote,  ///< On a `RemoteServer`, over a WebSocket.
};

/// @brief An application's bridge and what it needs to outlive: the worker pool of a local link.
///
/// Built by `connect()` inside an application factory, on the frontend's owner executor, and destroyed there
/// before the runtime. The bridge's backend belongs to the connection: do not `switchBackend` it.
class Connection {
public:
    /// @param ctx The frontend's context: its executor owns the bridge and receives every callback.
    /// @param link Where the backend runs; a remote one reports its state through `ready()`.
    /// @param backend The backend. Owned.
    /// @param pool The workers @p backend runs on, when it is local; destroyed after the bridge.
    Connection(ui::AppContext& ctx, Link link, std::unique_ptr<backend::detail::IBackend> backend,
               std::unique_ptr<exec::ThreadPoolExecutor> pool = nullptr);

    /// @brief Clears the remote backend's hooks, then destroys the bridge and only then the pool.
    ~Connection();
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    /// @brief The bridge every handler of the application is built on.
    /// @return The bridge, valid as long as this connection.
    [[nodiscard]] bridge::Bridge& bridge() noexcept { return *_bridge; }

    /// @brief The executor every handler delivers on: the frontend's owner.
    /// @return `ctx.executor()`.
    [[nodiscard]] exec::IExecutor& callbacks() noexcept { return *_callbacks; }

    /// @brief Whether calls can reach the models now. Tracked.
    ///
    /// Always true for a local link. A remote one turns true when its transport connects and false when it
    /// drops; a query whose key reads it issues nothing while it is false.
    /// @return The signal.
    [[nodiscard]] reactive::Signal<bool> const& ready() const noexcept { return _ready; }

    /// @brief Where the models run.
    /// @return The link given at construction.
    [[nodiscard]] Link link() const noexcept { return _link; }

private:
    std::unique_ptr<exec::ThreadPoolExecutor> _pool;
    exec::IExecutor* _callbacks;
    backend::detail::IBackend* _backend;
    std::unique_ptr<bridge::Bridge> _bridge;
    reactive::Signal<bool> _ready;
    Link _link;
    // Last, so it expires first: a notification posted from the transport's thread checks it before touching
    // `_ready`.
    std::shared_ptr<char> _alive = std::make_shared<char>();
};

/// @brief Builds the application's bridge.
///
/// Without `--server`: `setupDatabase(env.db)`, then a `LocalBackend` over a pool of `local.workers` (under
/// single-threaded WebAssembly, over `ctx.executor()`). With it: `QtWebSocketBackend` when the frontend is `"qt"`
/// (in a build with `MORPH_BUILD_QT`), `morph::net::SocketBackend` on `ctx.ioLoop()` when it is `"tui"` (in a
/// build with `MORPH_BUILD_NET`, which is POSIX-only). A non-empty `env.user` becomes the bridge's default
/// session principal. Call it on `ctx.executor()`, inside the application factory.
/// @param ctx The frontend's context.
/// @param env The deployment choices.
/// @param local How to run the models in-process.
/// @return The connection.
/// @throws TransportError when `--server` is given and no remote transport applies, naming why.
[[nodiscard]] std::unique_ptr<Connection> connect(ui::AppContext& ctx, AppEnvironment const& env, LocalSetup local);

}  // namespace morph::examples
```

Create `examples/common/app/transport.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "app/transport.hpp"

#include <format>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>

namespace morph::examples {

namespace {

std::unique_ptr<backend::detail::IBackend> remoteBackend(ui::AppContext& ctx, std::string const& url) {
    static_cast<void>(url);
    throw TransportError{std::format("--server is not supported on the '{}' frontend", ctx.frontendName())};
}

std::unique_ptr<Connection> localConnection(ui::AppContext& ctx, std::size_t workers) {
#if defined(__EMSCRIPTEN__)
    // One thread in a browser: the owner runs the models' strands too.
    static_cast<void>(workers);
    return std::make_unique<Connection>(ctx, Link::Local, std::make_unique<backend::LocalBackend>(ctx.executor()));
#else
    auto pool = std::make_unique<exec::ThreadPoolExecutor>(workers);
    auto backend = std::make_unique<backend::LocalBackend>(*pool);
    return std::make_unique<Connection>(ctx, Link::Local, std::move(backend), std::move(pool));
#endif
}

}  // namespace

Connection::Connection(ui::AppContext& ctx, Link link, std::unique_ptr<backend::detail::IBackend> backend,
                       std::unique_ptr<exec::ThreadPoolExecutor> pool)
    : _pool{std::move(pool)},
      _callbacks{&ctx.executor()},
      _backend{backend.get()},
      _bridge{std::make_unique<bridge::Bridge>(std::move(backend), ctx.executor())},
      _ready{ctx.runtime(), link == Link::Local},
      _link{link} {
    if (_link == Link::Local) {
        return;
    }
    // The hooks run on the transport's thread; the signal belongs to the owner, so the update is posted there and
    // dropped if this connection is gone by then.
    auto const notify = [owner = _callbacks, alive = std::weak_ptr<char>{_alive}, ready = &_ready](bool connected) {
        owner->post([alive, ready, connected] {
            if (!alive.expired()) {
                ready->set(connected);
            }
        });
    };
    _backend->setConnectHandler([notify] { notify(true); });
    _backend->setDisconnectHandler([notify] { notify(false); });
}

Connection::~Connection() {
    if (_link == Link::Remote) {
        _backend->setConnectHandler(nullptr);
        _backend->setDisconnectHandler(nullptr);
    }
}

std::unique_ptr<Connection> connect(ui::AppContext& ctx, AppEnvironment const& env, LocalSetup local) {
    std::unique_ptr<Connection> connection;
    if (env.server.has_value()) {
        connection = std::make_unique<Connection>(ctx, Link::Remote, remoteBackend(ctx, *env.server));
    } else {
        if (local.setupDatabase) {
            local.setupDatabase(env.db);
        }
        connection = localConnection(ctx, local.workers);
    }
    if (!env.user.empty()) {
        connection->bridge().setDefaultSession(session::Context{.principal = env.user});
    }
    return connection;
}

}  // namespace morph::examples
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[transport]"
```

Expected: PASS, 6 test cases. Mutation check: in the posted lambda, drop the `if (!alive.expired())` guard.
Expected FAIL in "a connect notification after the connection is gone touches nothing" — under the ASan build
(`cmake --preset clang-asan`, same target) as `heap-use-after-free` on the destroyed signal; a plain Debug build
may pass silently, which is why Task 12 runs this binary under ASan. Restore. Second: initialise `_ready` with
`true`. Expected FAIL in "a remote link is ready only between a connect and a drop". Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/common/app
git commit -m "wip(examples-common): Connection, connect() and the local transport

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: The remote transports — Qt Quick over `QtWebSocketBackend`, the TUI over `SocketBackend`

**Files:**
- Create: `examples/common/app/detail/remote_backends.hpp`, `examples/common/app/transport_qt.cpp`,
  `examples/common/app/transport_net.cpp`
- Modify: `examples/common/app/transport.cpp` — replace `remoteBackend` in the anonymous namespace
- Modify: `examples/common/app/CMakeLists.txt` — replace the `target_compile_definitions(… =0 … =0)` line from
  Task 5 with the block in Step 3
- Modify: `examples/common/app/tests/CMakeLists.txt` — the link block in Step 3
- Test: `examples/common/app/tests/test_transport.cpp` — append the cases below

**Interfaces:**
- Consumes: `qt::QtWebSocketBackend(QUrl, Config)` (`include/morph/qt/qt_websocket_backend.hpp:142`);
  `net::SocketBackend(exec::IoLoop&, std::string_view, Config = {})` (`include/morph/net/socket_backend.hpp:105`)
  and its connect/disconnect hooks (Task 1); `qt::QtWebSocketServer(RemoteServer&, quint16)`,
  `net::SocketServer(exec::IoLoop&, RemoteServer&, std::uint16_t)`, `backend::RemoteServer(IExecutor&)` (tests).
- Produces: `morph::examples::detail::qtBackend(std::string const&)`,
  `morph::examples::detail::socketBackend(exec::IoLoop&, std::string const&)`; the PUBLIC compile definitions
  `MORPH_EXAMPLES_TRANSPORT_QT` / `MORPH_EXAMPLES_TRANSPORT_NET` (`0`/`1`), which an application may read to say
  whether `--server` works on a frontend.

- [ ] **Step 1: Write the failing tests**

In `examples/common/app/tests/test_transport.cpp`, add `#include <array>`, `#include <format>` and
`#include <optional>` to the standard includes, and after the last `#include` line:

```cpp
#if MORPH_EXAMPLES_TRANSPORT_NET
#include <morph/core/io_loop.hpp>
#include <morph/core/remote.hpp>
#include <morph/net/socket_server.hpp>
#endif
#if MORPH_EXAMPLES_TRANSPORT_QT
#include <QCoreApplication>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#endif
```

Then append the cases:

```cpp
#if MORPH_EXAMPLES_TRANSPORT_NET
TEST_CASE("connect: --server on the terminal UI reaches a RemoteServer over morph::net, and follows a drop",
          "[examples-common][transport][remote]") {
    morph::exec::IoLoop ioLoop;
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::net::SocketServer>(ioLoop, *server, 0);
    REQUIRE(wsServer->listen());
    FakeAppContext ctx{"tui", &ioLoop};
    AppEnvironment env;
    env.server = std::format("ws://127.0.0.1:{}", wsServer->port());
    env.user = "ann";
    auto const connection = morph::examples::connect(ctx, env, LocalSetup{});
    CHECK(connection->link() == Link::Remote);
    CHECK(connection->bridge().defaultSession().principal == "ann");
    REQUIRE(pumpUntil(ctx.owner(), [&] { return connection->ready().peek(); }));

    morph::bridge::BridgeHandler<CommonPingModel> handler{connection->bridge(), &connection->callbacks()};
    CHECK(awaitOn(ctx.owner(), handler.execute(CommonPing{1})) == 2);

    wsServer.reset();
    CHECK(pumpUntil(ctx.owner(), [&] { return !connection->ready().peek(); }));
}

TEST_CASE("connect: a --server that is not a ws:// URL on the terminal UI is a TransportError naming it",
          "[examples-common][transport][remote]") {
    morph::exec::IoLoop ioLoop;
    FakeAppContext ctx{"tui", &ioLoop};
    AppEnvironment env;
    env.server = "http://example.invalid";
    CHECK_THROWS_MATCHES(morph::examples::connect(ctx, env, LocalSetup{}), TransportError,
                         Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("http://example.invalid")));
}

TEST_CASE("connect: --server on a terminal UI without an I/O loop is a TransportError",
          "[examples-common][transport][remote]") {
    FakeAppContext ctx{"tui"};
    AppEnvironment env;
    env.server = "ws://127.0.0.1:1";
    CHECK_THROWS_AS(morph::examples::connect(ctx, env, LocalSetup{}), TransportError);
}
#else
TEST_CASE("connect: --server on the terminal UI says morph::net is not built", "[examples-common][transport][remote]") {
    FakeAppContext ctx{"tui"};
    AppEnvironment env;
    env.server = "ws://127.0.0.1:1";
    CHECK_THROWS_MATCHES(morph::examples::connect(ctx, env, LocalSetup{}), TransportError,
                         Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("morph::net")));
}
#endif

#if MORPH_EXAMPLES_TRANSPORT_QT
TEST_CASE("connect: --server on Qt Quick reaches a RemoteServer over QtWebSocketBackend",
          "[examples-common][transport][remote]") {
    std::string program = "examples_common_app_tests";
    std::array<char*, 2> argv{program.data(), nullptr};
    int argc = 1;
    QCoreApplication const app{argc, argv.data()};
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer qtServer{*server, 0};
    REQUIRE(qtServer.listen());
    FakeAppContext ctx{"qt"};
    AppEnvironment env;
    env.server = std::format("ws://127.0.0.1:{}", qtServer.port());
    auto const connection = morph::examples::connect(ctx, env, LocalSetup{});
    // The socket lives on Qt's event loop and the replies on the owner: pump both.
    auto const pumpBoth = [&](auto done) {
        return pumpUntil(ctx.owner(), [&] {
            QCoreApplication::processEvents();
            return done();
        });
    };
    REQUIRE(pumpBoth([&] { return connection->ready().peek(); }));

    morph::bridge::BridgeHandler<CommonPingModel> handler{connection->bridge(), &connection->callbacks()};
    std::optional<int> reply;
    handler.execute(CommonPing{5}).then([&](int value) { reply = value; });
    REQUIRE(pumpBoth([&] { return reply.has_value(); }));
    CHECK(reply == std::optional<int>{6});
}

TEST_CASE("connect: a --server that is not a WebSocket URL on Qt Quick is a TransportError",
          "[examples-common][transport][remote]") {
    FakeAppContext ctx{"qt"};
    AppEnvironment env;
    env.server = "http://example.invalid";
    CHECK_THROWS_AS(morph::examples::connect(ctx, env, LocalSetup{}), TransportError);
}
#else
TEST_CASE("connect: --server on Qt Quick says morph::qt is not built", "[examples-common][transport][remote]") {
    FakeAppContext ctx{"qt"};
    AppEnvironment env;
    env.server = "ws://127.0.0.1:1";
    CHECK_THROWS_MATCHES(morph::examples::connect(ctx, env, LocalSetup{}), TransportError,
                         Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("morph::qt")));
}
#endif
```

- [ ] **Step 2: Run them to verify they fail**

Run:

```bash
cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[remote]"
```

Expected: FAIL — both macros are still `0`, so only the two `#else` cases compile, and they fail: the message
is `--server is not supported on the 'tui' frontend`, without `morph::net` (and likewise for `qt`).

- [ ] **Step 3: Implement**

Create `examples/common/app/detail/remote_backends.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/io_loop.hpp>
#include <string>

/// @file
/// @brief The remote backends `connect()` can build. Each is defined in its own translation unit, compiled only
///        when its transport is (`MORPH_EXAMPLES_TRANSPORT_QT`, `MORPH_EXAMPLES_TRANSPORT_NET`), so this header
///        names no toolkit.

namespace morph::examples::detail {

/// @brief A `QtWebSocketBackend` connecting to @p url. Defined in `transport_qt.cpp`.
/// @param url A `ws://` or `wss://` URL.
/// @return The backend, connecting.
/// @throws TransportError when @p url is not a WebSocket URL.
[[nodiscard]] std::unique_ptr<backend::detail::IBackend> qtBackend(std::string const& url);

/// @brief A `morph::net::SocketBackend` connecting to @p url on @p loop. Defined in `transport_net.cpp`.
/// @param loop The frontend's I/O loop. Borrowed: it must outlive the backend.
/// @param url A `ws://` URL.
/// @return The backend, connecting.
/// @throws TransportError when @p url is not a `ws://` URL.
[[nodiscard]] std::unique_ptr<backend::detail::IBackend> socketBackend(exec::IoLoop& loop, std::string const& url);

}  // namespace morph::examples::detail
```

Create `examples/common/app/transport_qt.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QString>
#include <QUrl>
#include <format>
#include <memory>
#include <morph/qt/qt_websocket_backend.hpp>
#include <string>

#include "app/detail/remote_backends.hpp"
#include "app/transport.hpp"

namespace morph::examples::detail {

std::unique_ptr<backend::detail::IBackend> qtBackend(std::string const& url) {
    QUrl const parsed{QString::fromStdString(url), QUrl::StrictMode};
    if (!parsed.isValid() || (parsed.scheme() != QStringLiteral("ws") && parsed.scheme() != QStringLiteral("wss"))) {
        throw TransportError{std::format("--server {}: not a ws:// or wss:// URL", url)};
    }
    return std::make_unique<qt::QtWebSocketBackend>(parsed, qt::QtWebSocketBackend::Config{});
}

}  // namespace morph::examples::detail
```

Create `examples/common/app/transport_net.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <exception>
#include <format>
#include <memory>
#include <morph/net/socket_backend.hpp>
#include <string>

#include "app/detail/remote_backends.hpp"
#include "app/transport.hpp"

namespace morph::examples::detail {

std::unique_ptr<backend::detail::IBackend> socketBackend(exec::IoLoop& loop, std::string const& url) {
    try {
        return std::make_unique<net::SocketBackend>(loop, url);
    } catch (std::exception const& error) {
        throw TransportError{std::format("--server {}: {}", url, error.what())};
    }
}

}  // namespace morph::examples::detail
```

In `examples/common/app/transport.cpp`, add `#include "app/detail/remote_backends.hpp"` and replace
`remoteBackend` with:

```cpp
std::unique_ptr<backend::detail::IBackend> remoteBackend(ui::AppContext& ctx, std::string const& url) {
    static_cast<void>(url);  // unused when neither transport is built
    auto const frontend = ctx.frontendName();
    if (frontend == "qt") {
#if MORPH_EXAMPLES_TRANSPORT_QT
        return detail::qtBackend(url);
#else
        throw TransportError{"--server on the Qt Quick frontend needs morph::qt, which this build does not include "
                             "(MORPH_BUILD_QT=OFF)"};
#endif
    }
    if (frontend == "tui") {
#if MORPH_EXAMPLES_TRANSPORT_NET
        if (ctx.ioLoop() == nullptr) {
            throw TransportError{"--server on the terminal UI needs the frontend's I/O loop, and this one has none"};
        }
        return detail::socketBackend(*ctx.ioLoop(), url);
#else
        throw TransportError{"--server on the terminal UI needs morph::net, which is POSIX-only and not built here "
                             "(MORPH_BUILD_NET=OFF, or Windows); run without --server for in-process models"};
#endif
    }
    throw TransportError{std::format("--server is not supported on the '{}' frontend", frontend)};
}
```

In `examples/common/app/CMakeLists.txt`, replace Task 5's `target_compile_definitions` line with:

```cmake
# The remote transports, each compiled in only when its option built it. Linked PRIVATE: their include paths
# (Qt's, core-cpp's sockets) never reach an application library. The two macros are PUBLIC and always 0 or 1,
# so code may test them under -Wundef.
set(_morph_examples_qt 0)
if(MORPH_BUILD_QT)
    set(_morph_examples_qt 1)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core WebSockets)
    target_sources(morph_ladder_app_common PRIVATE transport_qt.cpp)
    target_link_libraries(morph_ladder_app_common PRIVATE morph::qt morph_qt_impl Qt6::Core)
endif()
# morph::net is POSIX sockets: not on Windows, not in a browser.
set(_morph_examples_net 0)
if(MORPH_BUILD_NET AND NOT WIN32 AND NOT EMSCRIPTEN)
    set(_morph_examples_net 1)
    target_sources(morph_ladder_app_common PRIVATE transport_net.cpp)
    target_link_libraries(morph_ladder_app_common PRIVATE morph::net)
endif()
target_compile_definitions(morph_ladder_app_common PUBLIC
    MORPH_EXAMPLES_TRANSPORT_QT=${_morph_examples_qt}
    MORPH_EXAMPLES_TRANSPORT_NET=${_morph_examples_net})
```

In `examples/common/app/tests/CMakeLists.txt`, after `target_link_libraries(examples_common_app_tests …)`:

```cmake
# The remote cases stand up a server of each transport, so they need its headers directly.
if(MORPH_BUILD_QT)
    target_link_libraries(examples_common_app_tests PRIVATE morph::qt morph_qt_impl Qt6::Core)
endif()
if(MORPH_BUILD_NET AND NOT WIN32)
    target_link_libraries(examples_common_app_tests PRIVATE morph::net)
endif()
```

- [ ] **Step 4: Run the tests to verify they pass**

Run, in each configuration:
`cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[transport]"`
then the same with `build/ex-tui` and `build/ex-qt`.
Expected: PASS everywhere — `build/all` runs both round trips (11 `[transport]` cases), `build/ex-tui` the net
round trip and "says morph::qt is not built" (10), `build/ex-qt` the Qt round trip and "says morph::net is not
built" (9).
Mutation check: in `Connection`'s constructor, drop the `setDisconnectHandler` line. Expected FAIL in "reaches a
RemoteServer over morph::net, and follows a drop" (the last `pumpUntil` times out) and in Task 5's "ready only
between a connect and a drop" (`REQUIRE(seen.disconnect)`). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/common/app
git commit -m "wip(examples-common): remote transports over QtWebSocketBackend and SocketBackend

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: `Poller<Event, Cursor>` — "events since cursor" over a `Query`

`EventPoller` (`examples/common/gui/event_poller.hpp`) is a `QTimer` plus hand-sequenced in-flight flags. Its
replacement is a `Query` whose key is the cursor and whose `refreshEvery` is the poll interval, so the reactive
core supplies what `EventPoller` hand-rolled: one request at a time per key, a superseded reply dropped, a
destroyed poller gating replies in flight. Two decisions carry over: a `ClientTimeoutError` is retried on the
next tick (so is a `DisconnectedError`, which a reconnect ends), and any other failure stops polling and is
reported. One goes away: `EventPoller` set a bridge-wide execute deadline so a dropped reply could not wedge it;
here the next tick's refetch supersedes a reply that never came, so a poller touches no bridge setting.

**Files:**
- Create: `examples/common/app/poller.hpp`
- Modify: `examples/common/app/tests/CMakeLists.txt` — add `test_poller.cpp` after `test_transport.cpp`
- Test: `examples/common/app/tests/test_poller.cpp`

**Interfaces:**
- Consumes: `reactive::Query<A, R>(Runtime&, Fetch, Key, QueryOptions)`, `Query::{value, error, pending, refetch}`,
  `reactive::QueryOptions{scheduler, refreshEvery}`, `reactive::Effect`, `reactive::Signal`,
  `reactive::Runtime::{batch, untracked}`, `reactive::errorMessage` (Part 1);
  `backend::ClientTimeoutError`, `backend::DisconnectedError` (`core/backend.hpp:595`, `:626`);
  `async::Completion<T>::makeSettleable`.
- Produces: `morph::examples::PollPage<Event, Cursor>{events, next}`, `PollerOptions{interval}` †,
  `Poller<Event, Cursor>(reactive::Runtime&, reactive::Scheduler&, Fetch, Cursor start, OnEvent, PollerOptions = {})`
  with `cursor()`, `pending()`, `lastError()`, `stopped()`, `stoppedBy()`, `pollNow()`, `resume(Cursor)`, and the
  static `adapt(exec::IExecutor&, FetchFn, ToPageFn) -> Fetch` (a model reply → a page);
  `detail::isTransientPollError(std::exception_ptr const&)`. († addition to the contract.)

- [ ] **Step 1: Write the failing test**

Create `examples/common/app/tests/test_poller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/poller.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/completion.hpp>
#include <stdexcept>
#include <string>
#include <testkit/fake_app_context.hpp>
#include <utility>
#include <vector>

namespace {

using morph::async::Completion;
using morph::examples::PollerOptions;
using morph::examples::testing::FakeAppContext;
using namespace std::chrono_literals;

struct FeedEvent {
    int id = 0;
    std::string text;
};

using FeedPoller = morph::examples::Poller<FeedEvent, int>;
using FeedPage = FeedPoller::Page;

// Stands in for a server: every fetch is held until the test settles it.
class FakeFeed {
public:
    explicit FakeFeed(morph::exec::IExecutor& owner) : _owner{&owner} {}

    FeedPoller::Fetch fetcher() {
        return [this](int const& cursor) {
            auto [completion, promise] = Completion<FeedPage>::makeSettleable(_owner);
            _calls.push_back(Call{.cursor = cursor, .promise = std::move(promise)});
            return std::move(completion);
        };
    }

    void resolve(std::size_t index, FeedPage page) { _calls.at(index).promise.resolve(std::move(page)); }
    void reject(std::size_t index, std::exception_ptr error) { _calls.at(index).promise.reject(std::move(error)); }
    [[nodiscard]] std::size_t calls() const { return _calls.size(); }
    [[nodiscard]] int cursorOf(std::size_t index) const { return _calls.at(index).cursor; }

private:
    struct Call {
        int cursor;
        Completion<FeedPage>::Promise promise;
    };
    morph::exec::IExecutor* _owner;
    std::vector<Call> _calls;
};

struct Rig {
    FakeAppContext ctx;
    FakeFeed feed{ctx.owner()};
    std::vector<int> applied;
    std::unique_ptr<FeedPoller> poller = std::make_unique<FeedPoller>(
        ctx.runtime(), ctx.scheduler(), feed.fetcher(), 0, [this](FeedEvent const& event) { applied.push_back(event.id); },
        PollerOptions{.interval = 3000ms});

    void settle() { ctx.owner().drain(); }
};

FeedPage page(std::vector<int> const& ids, int next) {
    FeedPage out{.events = {}, .next = next};
    for (int const eventId : ids) {
        out.events.push_back(FeedEvent{.id = eventId, .text = "event " + std::to_string(eventId)});
    }
    return out;
}

}  // namespace

TEST_CASE("Poller: fetches from its start, applies a page in order and follows the cursor",
          "[examples-common][poller]") {
    Rig rig;
    REQUIRE(rig.feed.calls() == 1);
    CHECK(rig.feed.cursorOf(0) == 0);
    rig.feed.resolve(0, page({1, 2}, 2));
    rig.settle();
    CHECK(rig.applied == std::vector{1, 2});
    CHECK(rig.poller->cursor() == 2);
    REQUIRE(rig.feed.calls() == 2);  // a new cursor is a new key: asked at once, not on the next tick
    CHECK(rig.feed.cursorOf(1) == 2);
}

TEST_CASE("Poller: asks again on its interval while nothing happens", "[examples-common][poller]") {
    Rig rig;
    rig.feed.resolve(0, page({}, 0));
    rig.settle();
    CHECK(rig.feed.calls() == 1);
    rig.ctx.manualScheduler().advance(3000ms);
    rig.settle();
    REQUIRE(rig.feed.calls() == 2);
    CHECK(rig.feed.cursorOf(1) == 0);
}

TEST_CASE("Poller: a refetch while a page is in flight applies its events once", "[examples-common][poller]") {
    Rig rig;
    rig.ctx.manualScheduler().advance(3000ms);  // the first reply is late: the tick supersedes it
    rig.settle();
    REQUIRE(rig.feed.calls() == 2);
    CHECK(rig.feed.cursorOf(1) == 0);
    rig.feed.resolve(1, page({1}, 1));
    rig.settle();
    rig.feed.resolve(0, page({1}, 1));  // the superseded reply lands after all
    rig.settle();
    CHECK(rig.applied == std::vector{1});
    CHECK(rig.poller->cursor() == 1);
}

TEST_CASE("Poller: a timeout is retried on the next tick", "[examples-common][poller]") {
    Rig rig;
    rig.feed.reject(0, std::make_exception_ptr(morph::backend::ClientTimeoutError{}));
    rig.settle();
    CHECK_FALSE(rig.poller->stopped());
    CHECK(rig.poller->lastError() != nullptr);
    rig.ctx.manualScheduler().advance(3000ms);
    rig.settle();
    REQUIRE(rig.feed.calls() == 2);
    rig.feed.resolve(1, page({4}, 4));
    rig.settle();
    CHECK(rig.applied == std::vector{4});
    CHECK(rig.poller->lastError() == nullptr);
}

TEST_CASE("Poller: any other failure stops it and says why; resume restarts from a cursor",
          "[examples-common][poller]") {
    Rig rig;
    rig.feed.reject(0, std::make_exception_ptr(std::runtime_error{"poll is gone"}));
    rig.settle();
    REQUIRE(rig.poller->stopped());
    CHECK(morph::reactive::errorMessage(rig.poller->stoppedBy()) == "poll is gone");
    rig.ctx.manualScheduler().advance(30000ms);
    rig.settle();
    CHECK(rig.feed.calls() == 1);

    rig.poller->resume(7);
    rig.settle();
    CHECK_FALSE(rig.poller->stopped());
    REQUIRE(rig.feed.calls() == 2);
    CHECK(rig.feed.cursorOf(1) == 7);
}

TEST_CASE("Poller: destroying it cancels its timer and gates the reply in flight", "[examples-common][poller]") {
    Rig rig;
    rig.poller.reset();
    CHECK(rig.ctx.manualScheduler().pendingTimers() == 0);
    rig.feed.resolve(0, page({1}, 1));
    rig.settle();
    CHECK(rig.applied.empty());
}

TEST_CASE("Poller: adapt turns a model's reply into a page", "[examples-common][poller]") {
    struct Reply {
        std::vector<int> ids;
        int last = 0;
    };
    FakeAppContext ctx;
    std::vector<Completion<Reply>::Promise> promises;
    auto const fetch = FeedPoller::adapt(
        ctx.owner(),
        [&](int const&) {
            auto [completion, promise] = Completion<Reply>::makeSettleable(&ctx.owner());
            promises.push_back(std::move(promise));
            return std::move(completion);
        },
        [](Reply const& reply) {
            FeedPage out{.events = {}, .next = reply.last};
            for (int const eventId : reply.ids) {
                out.events.push_back(FeedEvent{.id = eventId, .text = {}});
            }
            return out;
        });
    std::vector<int> applied;
    FeedPoller const poller{ctx.runtime(), ctx.scheduler(), fetch, 0,
                            [&](FeedEvent const& event) { applied.push_back(event.id); }};
    REQUIRE(promises.size() == 1);
    promises.front().resolve(Reply{.ids = {5, 6}, .last = 6});
    ctx.owner().drain();
    CHECK(applied == std::vector{5, 6});
    CHECK(poller.cursor() == 6);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target examples_common_app_tests`
Expected: FAIL — `'app/poller.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/common/app/poller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief `morph::examples::Poller`: ask "what happened since this cursor" on an interval, apply the answer, and
///        move the cursor — a `Query` keyed by the cursor and refreshed by the frontend's scheduler.

namespace morph::examples {

/// @brief One answer to "what happened since this cursor".
/// @tparam Event An event of the feed.
/// @tparam Cursor The feed's position.
template <class Event, class Cursor>
struct PollPage {
    /// @brief The events after the asked cursor, oldest first.
    std::vector<Event> events;
    /// @brief The cursor after the last of them (the asked one when there are none).
    Cursor next;
};

/// @brief How a `Poller` paces itself.
struct PollerOptions {
    /// @brief How often to ask while nothing changes; zero asks only on a cursor change or `pollNow()`. Three
    ///        seconds trades a viewer's staleness against N viewers' request rate.
    std::chrono::milliseconds interval{3000};
};

namespace detail {

/// @brief Whether a poll failure is retried on the next tick: the client gave up waiting
///        (`ClientTimeoutError`) or the transport dropped (`DisconnectedError`, which a reconnect ends).
/// @param error The failure.
/// @return True for those two; false for anything else and for null.
[[nodiscard]] inline bool isTransientPollError(std::exception_ptr const& error) noexcept {
    if (error == nullptr) {
        return false;
    }
    try {
        std::rethrow_exception(error);
    } catch (backend::ClientTimeoutError const&) {
        return true;
    } catch (backend::DisconnectedError const&) {
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace detail

/// @brief Polls a feed for events since a cursor and applies each event once, in order.
///
/// A `Query` whose key is the cursor: a delivered page's events go to `onEvent`, then the cursor moves to
/// `page.next`, which is a new key and so is asked at once; while nothing changes the scheduler re-asks every
/// `interval`. A tick that finds a request still in flight supersedes it, so a reply that never comes cannot
/// wedge the poller, and a superseded reply is dropped. A transient failure (`detail::isTransientPollError`)
/// waits for the next tick; any other stops polling until `resume`. Owned by a controller, built and used on the
/// runtime's owner.
/// @tparam Event An event of the feed.
/// @tparam Cursor The feed's position; equality-comparable, so an unchanged cursor asks nothing new.
template <class Event, class Cursor>
class Poller {
public:
    /// @brief One answer from the feed.
    using Page = PollPage<Event, Cursor>;
    /// @brief Asks the feed for the events after a cursor; its `Completion` delivers on the runtime's owner.
    using Fetch = std::function<async::Completion<Page>(Cursor const&)>;
    /// @brief Applies one event.
    using OnEvent = std::function<void(Event const&)>;

    /// @param runtime The runtime. Borrowed: it must outlive the poller.
    /// @param scheduler Runs the interval. Borrowed: it must outlive the poller.
    /// @param fetch Asks the feed; `adapt` builds one over a model call.
    /// @param start The cursor to ask from first.
    /// @param onEvent Applies each event, oldest first, on the owner.
    /// @param options The interval.
    Poller(reactive::Runtime& runtime, reactive::Scheduler& scheduler, Fetch fetch, Cursor start, OnEvent onEvent,
           PollerOptions options = {})
        : _rt{&runtime},
          _cursor{runtime, std::move(start)},
          _stoppedBy{runtime, nullptr},
          _onEvent{std::move(onEvent)},
          _query{runtime, std::move(fetch),
                 [this]() -> std::optional<Cursor> {
                     if (_stoppedBy.get() != nullptr) {
                         return std::nullopt;
                     }
                     return _cursor.get();
                 },
                 reactive::QueryOptions{.scheduler = &scheduler, .refreshEvery = options.interval}},
          _apply{runtime, [this] { applyDelivered(); }},
          _watch{runtime, [this] { watchFailures(); }} {}

    ~Poller() = default;
    Poller(Poller const&) = delete;
    Poller& operator=(Poller const&) = delete;
    Poller(Poller&&) = delete;
    Poller& operator=(Poller&&) = delete;

    /// @brief Builds a `Fetch` over a call that answers with a model's own reply type.
    /// @tparam FetchFn Callable `(Cursor const&) -> async::Completion<R>`.
    /// @tparam ToPageFn Callable `(R const&) -> Page`; a throw fails that poll.
    /// @param owner The runtime's owner, where the page is delivered. Borrowed.
    /// @param fetch Issues the call.
    /// @param toPage Turns the reply into a page.
    /// @return The fetcher.
    template <class FetchFn, class ToPageFn>
    [[nodiscard]] static Fetch adapt(exec::IExecutor& owner, FetchFn fetch, ToPageFn toPage) {
        return [owner = &owner, fetch = std::move(fetch), toPage = std::move(toPage)](Cursor const& cursor) {
            auto [page, settle] = async::Completion<Page>::makeSettleable(owner);
            auto const promise = std::make_shared<typename async::Completion<Page>::Promise>(std::move(settle));
            auto reply = fetch(cursor);
            reply
                .then([promise, toPage](auto const& value) {
                    try {
                        promise->resolve(toPage(value));
                    } catch (...) {
                        promise->reject(std::current_exception());
                    }
                })
                .onError([promise](std::exception_ptr error) { promise->reject(std::move(error)); });
            return std::move(page);
        };
    }

    /// @brief Where the feed is read up to. Tracked.
    /// @return The cursor the next ask starts from.
    [[nodiscard]] Cursor const& cursor() const { return _cursor.get(); }

    /// @brief Whether an ask is in flight. Tracked.
    /// @return True from issue to delivery.
    [[nodiscard]] bool pending() const { return _query.pending(); }

    /// @brief The last transient failure, cleared by the next page. Tracked.
    /// @return The failure, or null.
    [[nodiscard]] std::exception_ptr lastError() const { return _query.error(); }

    /// @brief Whether a non-transient failure stopped polling. Tracked.
    /// @return True until `resume`.
    [[nodiscard]] bool stopped() const { return _stoppedBy.get() != nullptr; }

    /// @brief The failure that stopped polling. Tracked.
    /// @return The failure, or null while polling.
    [[nodiscard]] std::exception_ptr stoppedBy() const { return _stoppedBy.get(); }

    /// @brief Asks now instead of at the next tick; does nothing while stopped.
    void pollNow() { _query.refetch(); }

    /// @brief Starts polling again, from @p from.
    /// @param from The cursor to ask from.
    void resume(Cursor from) {
        _rt->batch([&] {
            _stoppedBy.set(nullptr);
            _cursor.set(std::move(from));
        });
    }

private:
    void applyDelivered() {
        auto const& page = _query.value();
        if (!page.has_value()) {
            return;
        }
        // The handler's own reads must not subscribe this effect to the application's signals.
        _rt->untracked([&] {
            for (auto const& event : page->events) {
                _onEvent(event);
            }
            _cursor.set(page->next);
        });
    }

    void watchFailures() {
        auto const error = _query.error();
        if (error == nullptr || detail::isTransientPollError(error)) {
            return;
        }
        _rt->untracked([&] {
            if (_stoppedBy.peek() == nullptr) {
                log::logError("Poller: polling stopped: {}", reactive::errorMessage(error));
                _stoppedBy.set(error);
            }
        });
    }

    reactive::Runtime* _rt;
    reactive::Signal<Cursor> _cursor;
    reactive::Signal<std::exception_ptr> _stoppedBy;
    OnEvent _onEvent;
    reactive::Query<Cursor, Page> _query;
    reactive::Effect _apply;
    reactive::Effect _watch;
};

}  // namespace morph::examples
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[poller]"
```

Expected: PASS, 7 test cases. Mutation check: in `applyDelivered`, apply the events before the
`untracked` call *and* inside it (apply twice). Expected FAIL in "applies a page in order" (`{1, 2, 1, 2}`).
Restore. Second: make `watchFailures` treat every error as fatal (drop the `isTransientPollError` test).
Expected FAIL in "a timeout is retried on the next tick" (`stopped()` is true). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/common/app
git commit -m "wip(examples-common): Poller, events since a cursor over a refreshed Query

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7b: `Wiring` and `mapCompletion` — what every controller is built from

Every migrated client's controllers take the same four borrowed collaborators, and every one derives a completion
from a handler's reply — mapping its value, observing its failure — on the owner and behind a `CallbackToken`.
Both live here once, in `morph::examples`, so Parts 7–10 include them instead of each app growing its own copy.
The implementation and the five tests are the kanban client's, renamed into `morph::examples`.

**Files:**
- Create: `examples/common/app/wiring.hpp`, `examples/common/app/completion_map.hpp`
- Modify: `examples/common/app/tests/CMakeLists.txt` — add `test_completion_map.cpp` after `test_poller.cpp`
- Test: `examples/common/app/tests/test_completion_map.cpp`

**Interfaces:**
- Consumes: `async::Completion<T>::makeSettleable(IExecutor*)`, `Completion::then(CallbackToken, …)`,
  `Completion::onError(CallbackToken, …)`, `Completion::thenDetached`, `Completion::onErrorDetached`
  (`morph/core/completion.hpp`); `async::CallbackScope::{token, reset}` (`morph/core/callback_scope.hpp`);
  `reactive::Runtime`, `reactive::Scheduler`, `reactive::errorMessage` (Part 1); `bridge::Bridge`;
  `testing::pumpUntil` (Task 4).
- Produces (the contract's Part 6 names):
  - `morph::examples::Wiring{runtime, scheduler, bridge, callbacks}` — `reactive::Runtime&`,
    `reactive::Scheduler&`, `bridge::Bridge&`, `exec::IExecutor&`, all borrowed (`app/wiring.hpp`).
  - `morph::examples::mapCompletion<To>(exec::IExecutor& owner, async::CallbackToken token,
    async::Completion<From> from, OnValue onValue, OnError onError) -> async::Completion<To>`
    (`app/completion_map.hpp`).

- [ ] **Step 1: Write the failing test**

Create `examples/common/app/tests/test_completion_map.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/completion_map.hpp>
#include <app/wiring.hpp>
#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <testkit/wait.hpp>
#include <type_traits>
#include <utility>

namespace {

using morph::async::Completion;
using morph::examples::testing::pumpUntil;

// Wiring borrows all four: a controller built from it owns none of them.
static_assert(std::is_same_v<decltype(morph::examples::Wiring::runtime), morph::reactive::Runtime&>);
static_assert(std::is_same_v<decltype(morph::examples::Wiring::scheduler), morph::reactive::Scheduler&>);
static_assert(std::is_same_v<decltype(morph::examples::Wiring::bridge), morph::bridge::Bridge&>);
static_assert(std::is_same_v<decltype(morph::examples::Wiring::callbacks), morph::exec::IExecutor&>);

// What a mapped completion delivered, observed from the outside.
struct Outcome {
    std::optional<std::string> value;
    std::string error;
};

void observe(Completion<std::string>& completion, Outcome& outcome) {
    completion.thenDetached([&outcome](std::string const& value) { outcome.value = value; })
        .onErrorDetached([&outcome](std::exception_ptr error) {
            outcome.error = morph::reactive::errorMessage(error);
        });
}

}  // namespace

TEST_CASE("mapCompletion maps a value on the owner", "[examples-common][completion]") {
    morph::exec::MainThreadExecutor owner;
    morph::async::CallbackScope scope;
    auto source = Completion<int>::makeSettleable(&owner);
    auto mapped = morph::examples::mapCompletion<std::string>(
        owner, scope.token(), std::move(source.first), [](int const& value) { return std::to_string(value * 2); },
        [](std::exception_ptr const&) {});
    Outcome outcome;
    observe(mapped, outcome);
    source.second.resolve(21);
    REQUIRE(pumpUntil(owner, [&outcome] { return outcome.value.has_value(); }));
    CHECK(*outcome.value == "42");
}

TEST_CASE("mapCompletion: two overlapping completions each deliver their own value",
          "[examples-common][completion]") {
    morph::exec::MainThreadExecutor owner;
    morph::async::CallbackScope scope;
    auto first = Completion<std::string>::makeSettleable(&owner);
    auto second = Completion<std::string>::makeSettleable(&owner);
    auto echo = [](std::string const& value) { return "created " + value; };
    auto mappedFirst = morph::examples::mapCompletion<std::string>(owner, scope.token(), std::move(first.first), echo,
                                                                   [](std::exception_ptr const&) {});
    auto mappedSecond = morph::examples::mapCompletion<std::string>(owner, scope.token(), std::move(second.first),
                                                                    echo, [](std::exception_ptr const&) {});
    Outcome firstOutcome;
    Outcome secondOutcome;
    observe(mappedFirst, firstOutcome);
    observe(mappedSecond, secondOutcome);
    second.second.resolve("Beta");
    first.second.resolve("Alpha");
    REQUIRE(pumpUntil(owner, [&] { return firstOutcome.value && secondOutcome.value; }));
    CHECK(*firstOutcome.value == "created Alpha");
    CHECK(*secondOutcome.value == "created Beta");
}

TEST_CASE("mapCompletion passes a failure on after observing it", "[examples-common][completion]") {
    morph::exec::MainThreadExecutor owner;
    morph::async::CallbackScope scope;
    auto source = Completion<int>::makeSettleable(&owner);
    std::string observed;
    auto mapped = morph::examples::mapCompletion<std::string>(
        owner, scope.token(), std::move(source.first), [](int const&) { return std::string{"unused"}; },
        [&observed](std::exception_ptr const& error) { observed = morph::reactive::errorMessage(error); });
    Outcome outcome;
    observe(mapped, outcome);
    source.second.reject(std::make_exception_ptr(std::runtime_error{"refused"}));
    REQUIRE(pumpUntil(owner, [&outcome] { return !outcome.error.empty(); }));
    CHECK(observed == "refused");
    CHECK(outcome.error == "refused");
}

TEST_CASE("mapCompletion fails the derived completion when the mapper throws", "[examples-common][completion]") {
    morph::exec::MainThreadExecutor owner;
    morph::async::CallbackScope scope;
    auto source = Completion<int>::makeSettleable(&owner);
    auto mapped = morph::examples::mapCompletion<std::string>(
        owner, scope.token(), std::move(source.first),
        [](int const&) -> std::string { throw std::runtime_error{"unreadable reply"}; },
        [](std::exception_ptr const&) {});
    Outcome outcome;
    observe(mapped, outcome);
    source.second.resolve(1);
    REQUIRE(pumpUntil(owner, [&outcome] { return !outcome.error.empty(); }));
    CHECK(outcome.error == "unreadable reply");
}

TEST_CASE("mapCompletion runs nothing once its scope is stopped", "[examples-common][completion]") {
    morph::exec::MainThreadExecutor owner;
    morph::async::CallbackScope scope;
    auto source = Completion<int>::makeSettleable(&owner);
    bool mapperRan = false;
    auto mapped = morph::examples::mapCompletion<std::string>(
        owner, scope.token(), std::move(source.first),
        [&mapperRan](int const&) {
            mapperRan = true;
            return std::string{};
        },
        [](std::exception_ptr const&) {});
    Outcome outcome;
    observe(mapped, outcome);
    scope.reset();
    source.second.resolve(1);
    bool drained = false;
    owner.post([&drained] { drained = true; });
    REQUIRE(pumpUntil(owner, [&drained] { return drained; }));
    CHECK_FALSE(mapperRan);
    CHECK_FALSE(outcome.value.has_value());
}
```

The last case posts a sentinel after the resolve: the owner runs tasks in post order, so when the sentinel has run
the delivery has had its turn.

Add `test_completion_map.cpp` after `test_poller.cpp` in `examples/common/app/tests/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target examples_common_app_tests`
Expected: FAIL — `'app/completion_map.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/common/app/wiring.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>

/// @file
/// @brief `morph::examples::Wiring`: the collaborators every example controller is built from.

namespace morph::examples {

/// @brief What every controller of an example client is built from. All four are borrowed and outlive the
///        controllers.
struct Wiring {
    /// @brief The runtime every signal, query and mutation belongs to.
    reactive::Runtime& runtime;
    /// @brief Timers on the runtime's owner; polling runs on it.
    reactive::Scheduler& scheduler;
    /// @brief The bridge every handler registers on.
    bridge::Bridge& bridge;
    /// @brief The runtime's owner; every handler delivers its completions here.
    exec::IExecutor& callbacks;
};

}  // namespace morph::examples
```

Create `examples/common/app/completion_map.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <memory>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <utility>

/// @file
/// @brief `morph::examples::mapCompletion`: derive a completion from a handler's reply, on the owner.

namespace morph::examples {

/// @brief Derives a completion from @p from: @p onValue maps the value (and may act on it first), @p onError
///        observes a failure before it is passed on.
///
/// Both callbacks run on @p owner and are gated by @p token: once the token's scope is stopped neither runs and
/// the derived completion never settles. A throw from @p onValue fails the derived completion with it. Every call
/// carries its own state, so two overlapping calls cannot see each other's values.
/// @tparam To The derived value type.
/// @tparam From The source value type.
/// @tparam OnValue Callable `To(From const&)`.
/// @tparam OnError Callable `void(std::exception_ptr const&)`.
/// @param owner Where @p from delivers and the derived completion belongs. Borrowed.
/// @param token Gates both callbacks.
/// @param from The source; it must belong to @p owner.
/// @param onValue Maps the source's value.
/// @param onError Observes the source's failure.
/// @return The derived completion, owned by @p owner.
template <typename To, typename From, typename OnValue, typename OnError>
[[nodiscard]] async::Completion<To> mapCompletion(exec::IExecutor& owner, async::CallbackToken token,
                                                  async::Completion<From> from, OnValue onValue, OnError onError) {
    auto settleable = async::Completion<To>::makeSettleable(&owner);
    auto promise = std::make_shared<typename async::Completion<To>::Promise>(std::move(settleable.second));
    from.then(token,
              [promise, onValue = std::move(onValue)](From const& value) {
                  try {
                      promise->resolve(onValue(value));
                  } catch (...) {
                      promise->reject(std::current_exception());
                  }
              })
        .onError(token, [promise, onError = std::move(onError)](std::exception_ptr error) {
            onError(error);
            promise->reject(std::move(error));
        });
    return std::move(settleable.first);
}

}  // namespace morph::examples
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[completion]"
```

Expected: PASS, 5 test cases. Mutation check: in `mapCompletion`, drop the `token` argument from `from.then(...)`
(call the ungated `then(handler)`). Expected FAIL in "runs nothing once its scope is stopped" (`mapperRan` is
true). Restore. Second: in `onError`'s handler, delete `onError(error);`. Expected FAIL in "passes a failure on
after observing it" (`observed` is empty). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/common/app
git commit -m "wip(examples-common): Wiring and mapCompletion, what every example controller is built from

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: The frontend smoke harness

Spec 4 §7 rule 5: one smoke test per frontend per app — the application mounts and quits on the TUI and on Qt
Quick offscreen. `smokeRun` drives a real `tui::Frontend` over a `core::tui::MockTerminalOutput` (subclassed to keep
the text written) and a `ScriptedInputSource` with no input channel, or a real `qt_quick::Frontend` on the
`offscreen` platform; it quits through the frontend's own scheduler once the view is visibly mounted, or with
`kSmokeTimedOutExit` when the budget runs out. "Mounted" is observed, not assumed: on the TUI, visible text
reached the terminal; on Qt Quick, an item named `w<id>` (spec 3 §2) exists under a window's content item.

`qt_quick::Frontend::run` constructs its own `QGuiApplication`, so the Qt half refuses to run — with an error
saying so — in a process that already has a Qt application object (the ladder testkit's Qt `main` creates one).
Smoke tests therefore live in binaries whose `main` is `morph_test_main` (Task 9 gives every rung one).

**Files:**
- Create: `cmake/morph_example_app.cmake` (with `morph_example_frontends`; Task 9 adds `morph_add_example_ui`)
- Create: `examples/common/testkit/frontend_smoke.hpp`, `examples/common/testkit_src/frontend_smoke.cpp`
- Modify: `CMakeLists.txt` (root) — `include(${PROJECT_SOURCE_DIR}/cmake/morph_example_app.cmake)` directly above
  `# ── Demo executable ──`
- Modify: `examples/common/app/CMakeLists.txt` — the `morph_example_testkit` target, inside
  `if(MORPH_BUILD_TESTS AND NOT EMSCRIPTEN)` before `add_subdirectory(tests)`
- Modify: `examples/common/app/tests/CMakeLists.txt` — add `test_frontend_smoke.cpp` after `test_completion_map.cpp`, and
  link `morph::example_testkit`
- Test: `examples/common/app/tests/test_frontend_smoke.cpp`

**Interfaces:**
- Consumes: `ui::Application`, `ui::ApplicationFactory`, `ui::AppContext::{scheduler, quit}`, `ui::Frontend::run`,
  `ui::TimerHandle`, `ui::text`, `ui::column` (Part 2); `tui::Frontend(tui::FrontendConfig{terminal, input,
  mouse})` (Part 3, `morph/tui/frontend.hpp`); `qt_quick::Frontend(int&, char**)`, whose `run` constructs its own
  `QGuiApplication` (Part 4, `morph/qt_quick/frontend.hpp`); core-cpp `core::tui::MockTerminalOutput(int, int)`,
  `core::tui::Terminal(std::unique_ptr<TerminalOutput>)`, `core::tui::runtime::testing::ScriptedInputSource()`
  (default-constructed: no input channel);
  `ladder::testkit::detail::scaledDeadlineMs`.
- Produces: the contract's `SmokeFrontend{Tui, QtQuick}` and `runFrontendSmoke(ui::ApplicationFactory const&,
  SmokeFrontend)`, plus † `kSmokeTimedOutExit` (124), `SmokeResult{exitCode, viewBuilt, mounted, timedOut, error,
  screenText}`, `frontendBuilt(SmokeFrontend)`, `smokeRun(factory, frontend, budget = 5000ms)`; target
  `morph_example_testkit` / `morph::example_testkit`; CMake `morph_example_frontends(<target> <scope>)`.

- [ ] **Step 1: Write the failing test**

Create `examples/common/app/tests/test_frontend_smoke.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>
#include <stdexcept>
#include <string>
#include <testkit/frontend_smoke.hpp>
#include <utility>

namespace {

namespace ui = morph::ui;
using morph::examples::testing::frontendBuilt;
using morph::examples::testing::kSmokeTimedOutExit;
using morph::examples::testing::runFrontendSmoke;
using morph::examples::testing::SmokeFrontend;
using morph::examples::testing::smokeRun;

class TextApp final : public ui::Application {
public:
    explicit TextApp(std::string text) : _text{std::move(text)} {}
    [[nodiscard]] ui::Node view() override { return ui::text({.text = _text}); }

private:
    std::string _text;
};

class EmptyApp final : public ui::Application {
public:
    [[nodiscard]] ui::Node view() override { return ui::column({}); }
};

class SelfQuittingApp final : public ui::Application {
public:
    explicit SelfQuittingApp(ui::AppContext& ctx)
        : _leave{ctx.scheduler().after(std::chrono::milliseconds{0}, [&ctx] { ctx.quit(3); })} {}
    [[nodiscard]] ui::Node view() override { return ui::text({.text = "leaving"}); }

private:
    ui::TimerHandle _leave;
};

ui::ApplicationFactory textApp(std::string text) {
    return [text = std::move(text)](ui::AppContext&) { return std::make_unique<TextApp>(text); };
}

void skipUnlessBuilt(SmokeFrontend frontend) {
    if (!frontendBuilt(frontend)) {
        SKIP("this configure did not build that frontend");
    }
}

}  // namespace

TEST_CASE("runFrontendSmoke: a minimal application mounts and quits on the terminal UI", "[examples-common][smoke]") {
    runFrontendSmoke(textApp("smoke screen"), SmokeFrontend::Tui);
    auto const result = smokeRun(textApp("smoke screen"), SmokeFrontend::Tui);
    CHECK_THAT(result.screenText, Catch::Matchers::ContainsSubstring("smoke screen"));
}

TEST_CASE("runFrontendSmoke: a minimal application mounts and quits on Qt Quick", "[examples-common][smoke]") {
    runFrontendSmoke(textApp("smoke window"), SmokeFrontend::QtQuick);
}

TEST_CASE("smokeRun: an empty view times out instead of passing", "[examples-common][smoke]") {
    skipUnlessBuilt(SmokeFrontend::Tui);
    auto const result = smokeRun([](ui::AppContext&) { return std::make_unique<EmptyApp>(); }, SmokeFrontend::Tui,
                                 std::chrono::milliseconds{200});
    CHECK(result.viewBuilt);
    CHECK_FALSE(result.mounted);
    CHECK(result.timedOut);
    CHECK(result.exitCode == kSmokeTimedOutExit);
}

TEST_CASE("smokeRun: a factory that throws is reported, not hung", "[examples-common][smoke]") {
    auto const frontend = GENERATE(SmokeFrontend::Tui, SmokeFrontend::QtQuick);
    skipUnlessBuilt(frontend);
    auto const result = smokeRun(
        [](ui::AppContext&) -> std::unique_ptr<ui::Application> { throw std::runtime_error{"factory failed"}; },
        frontend);
    CHECK(result.error == "factory failed");
    CHECK_FALSE(result.viewBuilt);
}

TEST_CASE("smokeRun: the exit code is the one the application quit with", "[examples-common][smoke]") {
    auto const frontend = GENERATE(SmokeFrontend::Tui, SmokeFrontend::QtQuick);
    skipUnlessBuilt(frontend);
    auto const result = smokeRun([](ui::AppContext& ctx) { return std::make_unique<SelfQuittingApp>(ctx); }, frontend);
    CHECK(result.error.empty());
    CHECK(result.exitCode == 3);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/all --target examples_common_app_tests`
Expected: FAIL — `'testkit/frontend_smoke.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `cmake/morph_example_app.cmake`:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# Building an example application against whichever frontends this configure built. Included from the root
# CMakeLists.txt before the Demo block, so examples/bank, examples/tui and the ladder (through
# cmake/morph_add_rung.cmake) all reach it. Frontend targets are named, not tested with if(TARGET): they are
# resolved at generate time, wherever the root creates them.

# morph_example_frontends(<target> <PUBLIC|PRIVATE|INTERFACE>)
#
# Links every frontend that was built and defines MORPH_EXAMPLE_HAS_TUI and MORPH_EXAMPLE_HAS_QT_QUICK to 1 or 0
# (both always defined: -Wundef is on). The terminal UI is never built under Emscripten.
function(morph_example_frontends target scope)
    set(_tui 0)
    set(_qt_quick 0)
    if(MORPH_BUILD_TUI AND NOT EMSCRIPTEN)
        set(_tui 1)
        target_link_libraries(${target} ${scope} morph::tui)
    endif()
    if(MORPH_BUILD_QT_QUICK)
        set(_qt_quick 1)
        find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
        target_link_libraries(${target} ${scope} morph::qt_quick Qt6::Gui Qt6::Quick)
    endif()
    target_compile_definitions(${target} ${scope}
        MORPH_EXAMPLE_HAS_TUI=${_tui}
        MORPH_EXAMPLE_HAS_QT_QUICK=${_qt_quick})
endfunction()
```

In the root `CMakeLists.txt`, directly above `# ── Demo executable ──`:

```cmake
# Example applications build their one binary against whichever frontends were built (see the module).
include(${PROJECT_SOURCE_DIR}/cmake/morph_example_app.cmake)
```

In `examples/common/app/CMakeLists.txt`, inside `if(MORPH_BUILD_TESTS AND NOT EMSCRIPTEN)` and before
`add_subdirectory(tests)`:

```cmake
    # morph_example_testkit: the Qt-free waits and fake context (header-only) and the frontend smoke harness,
    # which links every built frontend. Its TU lives in testkit_src/, outside testkit/.clang-tidy's Catch2-only
    # suppression (see that file).
    add_library(morph_example_testkit STATIC
        "${PROJECT_SOURCE_DIR}/examples/common/testkit_src/frontend_smoke.cpp"
        "${PROJECT_SOURCE_DIR}/examples/common/testkit/frontend_smoke.hpp"
        "${PROJECT_SOURCE_DIR}/examples/common/testkit/fake_app_context.hpp"
        "${PROJECT_SOURCE_DIR}/examples/common/testkit/wait.hpp")
    add_library(morph::example_testkit ALIAS morph_example_testkit)
    target_include_directories(morph_example_testkit PUBLIC "${PROJECT_SOURCE_DIR}/examples/common")
    target_link_libraries(morph_example_testkit PUBLIC morph::morph Catch2::Catch2)
    target_compile_features(morph_example_testkit PUBLIC cxx_std_23)
    morph_example_frontends(morph_example_testkit PUBLIC)
    apply_warnings(morph_example_testkit)
    if(AF_COVERAGE)
        apply_coverage(morph_example_testkit)
    endif()
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(morph_example_testkit ${AF_SANITIZER})
    endif()
```

In `examples/common/app/tests/CMakeLists.txt`, change the link line to
`target_link_libraries(examples_common_app_tests PRIVATE morph::ladder_app_common morph::example_testkit morph_test_main)`.

Create `examples/common/testkit/frontend_smoke.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <morph/ui/frontend.hpp>
#include <string>

/// @file
/// @brief The frontend smoke test every example application carries: its factory, run on a real frontend,
///        mounts a view that reaches the screen and quits with 0.

namespace morph::examples::testing {

/// @brief Which real frontend a smoke run uses.
enum class SmokeFrontend : std::uint8_t {
    Tui,      ///< `morph::tui` over a mock terminal and a scripted input source with no input.
    QtQuick,  ///< `morph::qt_quick` on Qt's `offscreen` platform.
};

/// @brief The exit code a smoke run quits with when the view does not reach the screen within its budget.
inline constexpr int kSmokeTimedOutExit = 124;

/// @brief What one smoke run observed.
struct SmokeResult {
    int exitCode = -1;       ///< What `Frontend::run` returned; -1 when it threw.
    bool viewBuilt = false;  ///< The application's `view()` was called.
    bool mounted = false;    ///< The view reached the screen (TUI: visible text; Qt Quick: a `w<id>` item).
    bool timedOut = false;   ///< The budget ran out first, and the run quit with `kSmokeTimedOutExit`.
    std::string error;       ///< What escaped `run` (or why the run was refused); empty when it returned.
    std::string screenText;  ///< Every piece of text the TUI wrote; empty on Qt Quick.
};

/// @brief Whether this build includes @p frontend.
/// @param frontend The frontend.
/// @return True when its CMake option was on.
[[nodiscard]] bool frontendBuilt(SmokeFrontend frontend) noexcept;

/// @brief Runs @p factory on @p frontend until its view reaches the screen, then quits with 0.
///
/// The quit comes from the frontend's own scheduler, checked every 20 ms; the budget is scaled by
/// `MORPH_LADDER_DEADLINE_MS`. On Qt Quick, `QT_QPA_PLATFORM` defaults to `offscreen`, and a process that already
/// has a Qt application object is refused with an error, because the frontend constructs its own.
/// @param factory The application factory under test.
/// @param frontend The frontend.
/// @param budget How long the view may take to appear, before scaling.
/// @return What was observed.
[[nodiscard]] SmokeResult smokeRun(ui::ApplicationFactory const& factory, SmokeFrontend frontend,
                                   std::chrono::milliseconds budget = std::chrono::milliseconds{5000});

/// @brief The Catch2 smoke test: skips when @p frontend is not built, else asserts that the application built its
///        view, the view reached the screen, nothing escaped, and the run exited 0.
/// @param factory The application factory under test.
/// @param frontend The frontend.
void runFrontendSmoke(ui::ApplicationFactory const& factory, SmokeFrontend frontend);

}  // namespace morph::examples::testing
```

Create `examples/common/testkit_src/frontend_smoke.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "testkit/frontend_smoke.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <morph/ui/frontend.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "testkit/deadline.hpp"

#if MORPH_EXAMPLE_HAS_TUI
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TerminalOutput.hpp>
#include <core/tui/runtime/testing/ScriptedInputSource.hpp>
#include <morph/tui/frontend.hpp>
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QWindow>
#include <array>
#include <morph/qt_quick/frontend.hpp>
#endif

namespace morph::examples::testing {

namespace {

constexpr bool kHasTui = MORPH_EXAMPLE_HAS_TUI == 1;
constexpr bool kHasQtQuick = MORPH_EXAMPLE_HAS_QT_QUICK == 1;
constexpr std::chrono::milliseconds kTick{20};

// Wraps the application under test: records that its view was built, and owns the smoke's timer, so the timer
// dies with the application, before the frontend's runtime and scheduler.
class SmokeProbe final : public ui::Application {
public:
    SmokeProbe(std::unique_ptr<ui::Application> inner, SmokeResult& result)
        : _inner{std::move(inner)}, _result{&result} {}

    [[nodiscard]] ui::Node view() override {
        _result->viewBuilt = true;
        return _inner->view();
    }

    void arm(ui::TimerHandle timer) { _timer = std::move(timer); }

private:
    std::unique_ptr<ui::Application> _inner;
    SmokeResult* _result;
    ui::TimerHandle _timer;
};

SmokeResult drive(ui::Frontend& frontend, ui::ApplicationFactory const& factory, std::chrono::milliseconds budget,
                  std::function<bool()> const& mounted) {
    SmokeResult result;
    auto const limit = std::chrono::milliseconds{ladder::testkit::detail::scaledDeadlineMs(budget.count())};
    ui::ApplicationFactory const wrapped = [&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto inner = factory(ctx);
        if (!inner) {
            throw std::logic_error{"the application factory returned no application"};
        }
        auto probe = std::make_unique<SmokeProbe>(std::move(inner), result);
        auto const started = std::chrono::steady_clock::now();
        probe->arm(ctx.scheduler().every(
            kTick, [&ctx, &result, &mounted, started, limit, quitting = false]() mutable {
                if (quitting) {
                    return;
                }
                if (result.viewBuilt && mounted()) {
                    result.mounted = true;
                    quitting = true;
                    ctx.quit(0);
                } else if (std::chrono::steady_clock::now() - started >= limit) {
                    result.timedOut = true;
                    quitting = true;
                    ctx.quit(kSmokeTimedOutExit);
                }
            }));
        return probe;
    };
    try {
        result.exitCode = frontend.run(wrapped);
    } catch (std::exception const& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "a non-std::exception escaped Frontend::run";
    }
    return result;
}

#if MORPH_EXAMPLE_HAS_TUI
// Keeps the text the frontend writes, so "mounted" means the view's text reached the terminal.
class CapturingOutput final : public ::core::tui::MockTerminalOutput {
public:
    using MockTerminalOutput::MockTerminalOutput;

    void writeText(std::string_view text, ::core::tui::Style const& style) override {
        _text.append(text);
        MockTerminalOutput::writeText(text, style);
    }

    [[nodiscard]] std::string const& text() const noexcept { return _text; }

private:
    std::string _text;
};

SmokeResult runTui(ui::ApplicationFactory const& factory, std::chrono::milliseconds budget) {
    auto output = std::make_unique<CapturingOutput>(100, 40);
    CapturingOutput const* const screen = output.get();
    ::core::tui::Terminal terminal{std::move(output)};
    ::core::tui::runtime::testing::ScriptedInputSource input;  // no input channel: mount and quit only
    tui::Frontend frontend{tui::FrontendConfig{.terminal = &terminal, .input = &input}};
    auto result = drive(frontend, factory, budget, [screen] {
        return std::ranges::any_of(screen->text(), [](char glyph) { return glyph != ' ' && glyph != '\n'; });
    });
    result.screenText = screen->text();
    return result;
}
#else
SmokeResult runTui(ui::ApplicationFactory const& /*factory*/, std::chrono::milliseconds /*budget*/) {
    return SmokeResult{.error = "the terminal UI frontend is not built (MORPH_BUILD_TUI=OFF)"};
}
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
bool isWidgetName(QString const& name) {
    return name.size() > 1 && name.front() == QLatin1Char('w') &&
           std::all_of(name.cbegin() + 1, name.cend(), [](QChar glyph) { return glyph.isDigit(); });
}

bool hasWidgetItem(QQuickItem const& item) {
    auto const children = item.childItems();
    return std::ranges::any_of(children, [](QQuickItem const* child) {
        return isWidgetName(child->objectName()) || hasWidgetItem(*child);
    });
}

bool quickViewMounted() {
    auto const windows = QGuiApplication::topLevelWindows();
    return std::ranges::any_of(windows, [](QWindow* window) {
        auto const* quick = qobject_cast<QQuickWindow*>(window);
        return quick != nullptr && quick->contentItem() != nullptr && hasWidgetItem(*quick->contentItem());
    });
}

SmokeResult runQtQuick(ui::ApplicationFactory const& factory, std::chrono::milliseconds budget) {
    if (QCoreApplication::instance() != nullptr) {
        return SmokeResult{.error = "a Qt application object already exists, and the Qt Quick frontend constructs its "
                                    "own: run this smoke from a binary whose main creates none (morph_test_main)"};
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    std::string program = "frontend_smoke";
    std::array<char*, 2> argv{program.data(), nullptr};
    int argc = 1;
    qt_quick::Frontend frontend{argc, argv.data()};
    return drive(frontend, factory, budget, [] { return quickViewMounted(); });
}
#else
SmokeResult runQtQuick(ui::ApplicationFactory const& /*factory*/, std::chrono::milliseconds /*budget*/) {
    return SmokeResult{.error = "the Qt Quick frontend is not built (MORPH_BUILD_QT_QUICK=OFF)"};
}
#endif

}  // namespace

bool frontendBuilt(SmokeFrontend frontend) noexcept {
    return frontend == SmokeFrontend::Tui ? kHasTui : kHasQtQuick;
}

SmokeResult smokeRun(ui::ApplicationFactory const& factory, SmokeFrontend frontend, std::chrono::milliseconds budget) {
    return frontend == SmokeFrontend::Tui ? runTui(factory, budget) : runQtQuick(factory, budget);
}

void runFrontendSmoke(ui::ApplicationFactory const& factory, SmokeFrontend frontend) {
    if (!frontendBuilt(frontend)) {
        SKIP((frontend == SmokeFrontend::Tui ? "the terminal UI frontend is not built"
                                             : "the Qt Quick frontend is not built"));
    }
    auto const result = smokeRun(factory, frontend);
    INFO("error: " << result.error);
    REQUIRE(result.error.empty());
    CHECK(result.viewBuilt);
    CHECK(result.mounted);
    CHECK_FALSE(result.timedOut);
    CHECK(result.exitCode == 0);
}

}  // namespace morph::examples::testing
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/all --target examples_common_app_tests && ./build/all/examples/common/app/tests/examples_common_app_tests "[smoke]"
```

Expected: PASS, 5 test cases, none skipped on `build/all`. On `build/ex-tui` the Qt Quick generator rows and the
Qt Quick case report as skipped, and on `build/ex-qt` the TUI ones. Mutation check: in `drive`, replace
`result.viewBuilt && mounted()` with `result.viewBuilt`. Expected FAIL in "an empty view times out instead of
passing" (`mounted` is true, exit 0). Restore. Second: make `isWidgetName` return `false`. Expected FAIL in
"a minimal application mounts and quits on Qt Quick" (`mounted` false, the run times out with 124). Restore.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt cmake/morph_example_app.cmake examples/common/testkit/frontend_smoke.hpp \
        examples/common/testkit_src/frontend_smoke.cpp examples/common/app
git commit -m "wip(examples-common): the frontend smoke harness

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: `morph_add_example_ui` and `morph_add_rung`'s `app/` + `ui/` layout

Spec 4 §2 and §6: a rung's `app/*.cpp` becomes `ladder_<rung>_app` (morph, the rung's library and
`morph_ladder_app_common` — no toolkit), its `ui/*.cpp` the one binary `<rung>`, built when at least one frontend
is and linking every frontend built. One more convention follows from Task 8: `tests/smoke/*.cpp` becomes
`ladder_<rung>_smoke_tests`, whose `main` is `morph_test_main`, because the rung's ordinary test binary owns a
`QCoreApplication` and the Qt Quick frontend constructs its own. Every existing convention keeps working
unchanged; no rung has `app/`, `ui/` or `tests/smoke/` yet, so this task changes no existing target, and the new
blocks are proven on a probe rung layout that is created, built and deleted in Step 4.

**Files:**
- Modify: `cmake/morph_example_app.cmake` — append `morph_add_example_ui`
- Modify: `cmake/morph_add_rung.cmake` — the header's directory table; a helper before `function(morph_add_rung)`;
  a `ladder_<rung>_app` block after the `ladder_<rung>_lib` block; a `<rung>` block after it; in the tests block,
  the smoke exclusion and the app link; a `ladder_<rung>_smoke_tests` block after the tests block

**Interfaces:**
- Consumes: `morph_example_frontends` (Task 8); targets `morph::ladder_app_common`, `morph::example_testkit`,
  `morph_test_main`; `apply_warnings`, `apply_bigobj`, `apply_coverage`, `apply_sanitizers`.
- Produces: `morph_add_example_ui(TARGET <name> SOURCES <src>... LIBRARIES <item>...)` (rungs and `examples/tui`; bank links `morph_example_frontends` directly, because the helper applies the strict warning set and bank's model headers include the ORM's, which are not `-Werror` clean);
  per rung, when the directories exist: `ladder_<rung>_app` / `morph::ladder_<rung>_app` (PUBLIC include dirs
  `<rung>/app` and `<rung>/include`), executable `<rung>`, `ladder_<rung>_smoke_tests` (ctest prefix
  `<rung>.smoke.`, label `ladder-<rung>`); `ladder_<rung>_tests` additionally links `ladder_<rung>_app` and
  `morph::example_testkit` and no longer compiles `tests/smoke/`.

- [ ] **Step 1: Write the failing check**

The deliverable is CMake, so the failing check is a probe rung layout. Create these three files (they are
deleted in Step 4 and never committed):

`examples/pastebin/app/probe_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/app_environment.hpp>
#include <memory>
#include <morph/ui/frontend.hpp>

namespace pastebin_probe::client {
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env);
}
```

`examples/pastebin/app/probe_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "probe_application.hpp"

#include <morph/ui/view.hpp>

namespace pastebin_probe::client {
namespace {
class Probe final : public morph::ui::Application {
public:
    [[nodiscard]] morph::ui::Node view() override { return morph::ui::text({.text = "pastebin probe"}); }
};
}  // namespace

std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& /*ctx*/,
                                                        morph::examples::AppEnvironment const& /*env*/) {
    return std::make_unique<Probe>();
}
}  // namespace pastebin_probe::client
```

`examples/pastebin/ui/main.cpp` (the shape every example's `main` has, spec 4 §3):

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <vector>

#include "probe_application.hpp"

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
        return frontend->run(
            [&env](morph::ui::AppContext& ctx) { return pastebin_probe::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "pastebin: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "pastebin: unknown error\n";
    }
    return 1;
}
```

`examples/pastebin/tests/smoke/test_probe_smoke.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/frontend.hpp>
#include <testkit/frontend_smoke.hpp>

#include "probe_application.hpp"

using morph::examples::AppEnvironment;
using morph::examples::testing::runFrontendSmoke;
using morph::examples::testing::SmokeFrontend;

TEST_CASE("pastebin probe: mounts and quits on the terminal UI", "[probe][smoke]") {
    runFrontendSmoke(
        [](morph::ui::AppContext& ctx) { return pastebin_probe::client::makeApplication(ctx, AppEnvironment{}); },
        SmokeFrontend::Tui);
}

TEST_CASE("pastebin probe: mounts and quits on Qt Quick", "[probe][smoke]") {
    runFrontendSmoke(
        [](morph::ui::AppContext& ctx) { return pastebin_probe::client::makeApplication(ctx, AppEnvironment{}); },
        SmokeFrontend::QtQuick);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake -S . -B build/all && cmake --build build/all --target ladder_pastebin_app`
Expected: FAIL — `ninja: error: unknown target 'ladder_pastebin_app'`. (Building `ladder_pastebin_tests` now would
also fail: its glob picks up `tests/smoke/test_probe_smoke.cpp`, which cannot find `probe_application.hpp`.)

- [ ] **Step 3: Implement**

Append to `cmake/morph_example_app.cmake`:

```cmake
# morph_add_example_ui(TARGET <name> SOURCES <src>... LIBRARIES <item>...)
#
# An example's one binary: its main picks a frontend at runtime among those built. Built when at least one
# frontend is, linking every one that was, with MORPH_EXAMPLE_HAS_TUI / MORPH_EXAMPLE_HAS_QT_QUICK defined for
# main's #if. With Qt Quick it is a Qt executable, which is what a WebAssembly build of the same main needs. A
# skip is announced, never silent: a target that vanishes from a successful configure surfaces only later, as
# "unknown target".
function(morph_add_example_ui)
    cmake_parse_arguments(UI "" "TARGET" "SOURCES;LIBRARIES" ${ARGN})
    if(NOT UI_TARGET OR NOT UI_SOURCES)
        message(FATAL_ERROR "morph_add_example_ui() requires TARGET and SOURCES")
    endif()
    if(NOT ((MORPH_BUILD_TUI AND NOT EMSCRIPTEN) OR MORPH_BUILD_QT_QUICK))
        message(STATUS "morph_add_example_ui: ${UI_TARGET} is not built -- it needs MORPH_BUILD_TUI "
                       "or MORPH_BUILD_QT_QUICK")
        return()
    endif()
    if(MORPH_BUILD_QT_QUICK)
        find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
        qt_add_executable(${UI_TARGET} ${UI_SOURCES})
    else()
        add_executable(${UI_TARGET} ${UI_SOURCES})
    endif()
    target_link_libraries(${UI_TARGET} PRIVATE ${UI_LIBRARIES})
    morph_example_frontends(${UI_TARGET} PRIVATE)
    target_compile_features(${UI_TARGET} PRIVATE cxx_std_23)
    apply_warnings(${UI_TARGET})
    apply_bigobj(${UI_TARGET})
    if(AF_COVERAGE)
        apply_coverage(${UI_TARGET})
    endif()
    # Every library it links is instrumented on a sanitizer preset; an uninstrumented binary fails to link.
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(${UI_TARGET} ${AF_SANITIZER})
    endif()
endfunction()
```

In `cmake/morph_add_rung.cmake`, extend the header's directory table after the `src/headless/*.cpp` line:

```cmake
#   app/*.cpp                                       -> ladder_<rung>_app      STATIC (controllers + views: morph, the rung's lib and morph_ladder_app_common; no toolkit)
#   ui/*.cpp                                        -> <rung>                 EXE    (the one binary; needs MORPH_BUILD_TUI or MORPH_BUILD_QT_QUICK; Qt Quick only under Emscripten)
#   tests/smoke/*.cpp                               -> ladder_<rung>_smoke_tests EXE (frontend smoke tests; a main that owns no Qt application object)
```

Before `function(morph_add_rung)`:

```cmake
# Lightweight's target_include_directories() is plain PUBLIC, not SYSTEM, so a target that sees its headers
# transitively and calls apply_warnings() would apply -Werror to them. This demotes them to SYSTEM for one
# target -- the same remedy the gui_lib and tests blocks below apply inline.
function(_morph_rung_system_lightweight target scope)
    get_target_property(_includes Lightweight::Lightweight INTERFACE_INCLUDE_DIRECTORIES)
    if(_includes)
        target_include_directories(${target} SYSTEM ${scope} ${_includes})
    endif()
endfunction()
```

After the `ladder_<rung>_lib` block's closing `endif()` (the `if(NOT EMSCRIPTEN)` one), add:

```cmake
    # ── ladder_<rung>_app: controllers and views, toolkit-free ───────────
    # The application a frontend runs. Its own link set is morph, the rung's
    # library and morph_ladder_app_common, which names no toolkit, so a Qt or
    # frontend include in app/ has nothing to resolve against here. Built under
    # Emscripten too, where there is no ladder_<rung>_lib: the browser binary is
    # the same ui/ main over the same library, with the models' headers only.
    file(GLOB_RECURSE _app_sources CONFIGURE_DEPENDS "${_dir}/app/*.cpp")
    if(_app_sources)
        if(NOT TARGET morph_ladder_app_common)
            message(FATAL_ERROR "morph_add_rung(NAME ${_rung}): app/ needs morph_ladder_app_common, which the root "
                                "CMakeLists.txt adds under MORPH_BUILD_LADDER -- configure from the root.")
        endif()
        add_library(ladder_${_rung}_app STATIC ${_app_sources})
        add_library(morph::ladder_${_rung}_app ALIAS ladder_${_rung}_app)
        target_include_directories(ladder_${_rung}_app PUBLIC "${_dir}/app" "${_dir}/include")
        target_link_libraries(ladder_${_rung}_app PUBLIC morph::morph morph::ladder_app_common)
        if(TARGET ladder_${_rung}_lib)
            target_link_libraries(ladder_${_rung}_app PUBLIC morph::ladder_${_rung}_lib)
            _morph_rung_system_lightweight(ladder_${_rung}_app PUBLIC)
        endif()
        target_compile_features(ladder_${_rung}_app PUBLIC cxx_std_23)
        apply_warnings(ladder_${_rung}_app)
        apply_bigobj(ladder_${_rung}_app)
        if(AF_COVERAGE)
            apply_coverage(ladder_${_rung}_app)
        endif()
        # See ladder_${_rung}_lib's identical AF_SANITIZER block above.
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(ladder_${_rung}_app ${AF_SANITIZER})
        endif()
    endif()

    # ── <rung>: the one binary, native and WebAssembly ───────────────────
    # The rung's library is linked whole so its schema TU's static-init
    # migrations reach the in-process database (see ladder_<rung>_tests'
    # WHOLE_ARCHIVE note below). A browser build dispatches every action to a
    # server, so it needs MORPH_CLIENT_ONLY for the reason ladder_<rung>_gui_wasm
    # does.
    file(GLOB _ui_sources CONFIGURE_DEPENDS "${_dir}/ui/*.cpp")
    if(_ui_sources AND NOT TARGET ladder_${_rung}_app)
        message(STATUS "morph_add_rung: rung '${_rung}' has ui/*.cpp but no app/*.cpp -- '${_rung}' is skipped")
    elseif(_ui_sources)
        if(EMSCRIPTEN AND NOT MORPH_CLIENT_ONLY)
            message(FATAL_ERROR
                "morph_add_rung: rung '${_rung}' builds '${_rung}' for WebAssembly, which needs "
                "-DMORPH_CLIENT_ONLY=ON. See docs/spec/core/registry.md, \"MORPH_CLIENT_ONLY\".")
        endif()
        set(_ui_libraries morph::ladder_${_rung}_app)
        if(TARGET ladder_${_rung}_lib)
            list(APPEND _ui_libraries "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph::ladder_${_rung}_lib>")
        endif()
        morph_add_example_ui(TARGET ${_rung} SOURCES ${_ui_sources} LIBRARIES ${_ui_libraries})
    endif()
```

In the `ladder_<rung>_tests` block, directly after `file(GLOB_RECURSE _test_sources …)`:

```cmake
        # tests/smoke/ is its own binary (below): its main must own no Qt application object.
        list(FILTER _test_sources EXCLUDE REGEX "/tests/smoke/")
```

and after the `if(TARGET ladder_${_rung}_gui_lib) … endif()` that links the gui_lib into the tests:

```cmake
            # Controller and view tests: the app library, and the Qt-free waits and fake context.
            if(TARGET ladder_${_rung}_app)
                target_link_libraries(ladder_${_rung}_tests PRIVATE morph::ladder_${_rung}_app morph::example_testkit)
            endif()
```

After the tests block's closing `endif()` (the `if(NOT EMSCRIPTEN)` around `ladder_<rung>_tests`), add:

```cmake
    # ── ladder_<rung>_smoke_tests: the app on each real frontend ─────────
    # A binary of its own because the Qt Quick frontend constructs its own
    # QGuiApplication, and ladder_<rung>_tests' main already owns a
    # QCoreApplication; morph_test_main owns none. Same database lock and label
    # as the rung's other tests; the "<rung>.smoke." prefix keeps the ctest names
    # apart from them.
    file(GLOB_RECURSE _smoke_sources CONFIGURE_DEPENDS "${_dir}/tests/smoke/*.cpp")
    if(NOT EMSCRIPTEN AND _smoke_sources AND TARGET ladder_${_rung}_app)
        add_executable(ladder_${_rung}_smoke_tests ${_smoke_sources})
        target_link_libraries(ladder_${_rung}_smoke_tests PRIVATE
            morph::ladder_${_rung}_app morph::example_testkit morph_test_main)
        if(TARGET ladder_${_rung}_lib)
            target_link_libraries(ladder_${_rung}_smoke_tests PRIVATE
                "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph::ladder_${_rung}_lib>")
            _morph_rung_system_lightweight(ladder_${_rung}_smoke_tests PRIVATE)
        endif()
        target_compile_features(ladder_${_rung}_smoke_tests PRIVATE cxx_std_23)
        apply_warnings(ladder_${_rung}_smoke_tests)
        apply_bigobj(ladder_${_rung}_smoke_tests)
        if(AF_COVERAGE)
            apply_coverage(ladder_${_rung}_smoke_tests)
        endif()
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(ladder_${_rung}_smoke_tests ${AF_SANITIZER})
        endif()
        include(Catch)
        get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
        cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)
        catch_discover_tests(ladder_${_rung}_smoke_tests
            DISCOVERY_MODE POST_BUILD
            TEST_PREFIX "${_rung}.smoke."
            DL_PATHS "${_qt_bin_dir}"
            PROPERTIES LABELS ladder-${_rung} TIMEOUT 120 RESOURCE_LOCK morph_ladder_test_db)
    elseif(_smoke_sources AND NOT TARGET ladder_${_rung}_app)
        message(STATUS "morph_add_rung: rung '${_rung}' has tests/smoke/ but no app/*.cpp -- "
                       "ladder_${_rung}_smoke_tests is skipped")
    endif()
```

- [ ] **Step 4: Run the check, then remove the probe**

```bash
cmake -S . -B build/all
cmake --build build/all --target ladder_pastebin_app pastebin ladder_pastebin_tests ladder_pastebin_smoke_tests
ctest --test-dir build/all -R '^pastebin\.smoke\.' --output-on-failure
./build/all/examples/pastebin/ladder_pastebin_tests --list-tests | grep -c 'pastebin probe'
ctest --test-dir build/all -N -L ladder-pastebin | grep -c 'pastebin.smoke.'
cmake --build build/ex-tui --target help | grep -c 'ladder_pastebin'
```

Expected: the build succeeds; the two smoke cases pass (`100% tests passed, 0 tests failed out of 2`); the
`--list-tests` count is `0` (the smoke file is not in the ordinary binary); the label count is `2`; the last
count is `0` (no ladder in `build/ex-tui`, and nothing about the rung layout leaks into a non-ladder configure).
Mutation check: delete the `list(FILTER … "/tests/smoke/")` line, rebuild `ladder_pastebin_tests` and run
`./build/all/examples/pastebin/ladder_pastebin_tests "[probe]"`. Expected FAIL in "pastebin probe: mounts and
quits on Qt Quick" with "a Qt application object already exists" — the testkit's Qt `main` owns a
`QCoreApplication`, which is why smoke tests get their own binary. Restore.

Then remove the probe and confirm the tree is as before:

```bash
rm -r examples/pastebin/app examples/pastebin/ui examples/pastebin/tests/smoke
cmake -S . -B build/all && cmake --build build/all
git status --short examples/pastebin   # prints nothing
```

- [ ] **Step 5: Commit**

```bash
git add cmake/morph_example_app.cmake cmake/morph_add_rung.cmake
git commit -m "wip(examples-common): morph_add_example_ui, and morph_add_rung's app/, ui/ and tests/smoke/

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: `examples/tui/gallery` — every node kind once, drag-and-drop included

Spec 1 §8: a visual check for backend authors, running through `ui::selectFrontend` like every migrated app.
Its state lives in signals on the application; every control writes a line into "Last event", which is what the
view test reads back. It is registered from the root's "Example applications on an injected frontend" block (Task
2), not the Demo block: its tests call `include(Catch)` and link `morph_example_testkit`, both of which exist only
after the Tests block — the reason is stated in the squashed commit's body as a deviation from the plan's brief.

**Files:**
- Create: `examples/tui/CMakeLists.txt`, `examples/tui/README.md`
- Create: `examples/tui/gallery/CMakeLists.txt`, `examples/tui/gallery/app/gallery_application.hpp`,
  `examples/tui/gallery/app/gallery_application.cpp`, `examples/tui/gallery/ui/main.cpp`
- Create: `examples/tui/gallery/tests/.clang-tidy` (Task 2's content), `tests/test_gallery_view.cpp`,
  `tests/test_gallery_smoke.cpp`
- Modify: `CMakeLists.txt` (root) — append to the "Example applications on an injected frontend" block

**Interfaces:**
- Consumes: every builder of `ui/view.hpp` (Part 2 contract); `ui::Mounted`;
  `ui::testing::RecordingBackend::{find, all, prop, exists, click, edit, choose, dismiss, drag}`;
  `ui::selectFrontend`, `ui::FrontendOption`; `tui::frontendOption(tui::FrontendConfig = {})`
  (`morph/tui/frontend.hpp`, Part 3), `qt_quick::frontendOption(int&, char**, ui::EnvironmentReader =
  ui::processEnvironment())` (`morph/qt_quick/frontend.hpp`, Part 4); `reactive::Signal::{get, peek, set, mutate}`;
  `examples::AppEnvironment::fromArgs` (Task 2); `FakeAppContext`, `runFrontendSmoke` (Tasks 4, 8);
  `morph_add_example_ui` (Task 9).
- Produces: `gallery::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&) ->
  std::unique_ptr<ui::Application>` (the gallery reads nothing from the environment); targets `gallery_app`,
  `gallery`, `gallery_tests` (label `examples-tui`).

- [ ] **Step 1: Write the failing tests**

Create `examples/tui/gallery/tests/test_gallery_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string_view>
#include <testkit/fake_app_context.hpp>

#include "gallery_application.hpp"

namespace {

using morph::examples::AppEnvironment;
using morph::examples::testing::FakeAppContext;
using morph::ui::testing::RecordingBackend;

struct Rig {
    FakeAppContext ctx;
    std::unique_ptr<morph::ui::Application> app = gallery::client::makeApplication(ctx, AppEnvironment{});
    RecordingBackend rec;
    morph::ui::Mounted mounted{ctx.runtime(), rec, app->view()};

    void settle() { ctx.owner().drain(); }

    int widget(std::string_view kind, std::string_view prop, std::string_view value) {
        auto const found = rec.find(kind, prop, value);
        REQUIRE(found.has_value());
        return *found;
    }

    [[nodiscard]] bool shows(std::string_view text) const { return rec.find("Text", "text", text).has_value(); }
};

}  // namespace

TEST_CASE("gallery: the view mounts every widget kind", "[gallery][view]") {
    Rig rig;
    rig.settle();
    // A stack is a Column or a Row by its axis (a ForEach is a Column); a Switch and each shown Tabs page are
    // Slots; a closed Dialog is still a Dialog, only its content is absent.
    constexpr std::array<std::string_view, 20> kKinds{
        "Text",  "Button", "TextInput", "Checkbox", "Select", "Menu", "Column", "Row",           "Grid",   "Spacer",
        "Panel", "Scroll", "Slot",      "Tabs",     "Dialog", "Busy", "Table",  "DateTimeInput", "Slider", "FilePicker"};
    for (auto const kind : kKinds) {
        INFO(kind);
        CHECK_FALSE(rig.rec.all(kind).empty());
    }
}

TEST_CASE("gallery: a click lands in the event line", "[gallery][view]") {
    Rig rig;
    rig.settle();
    CHECK(rig.shows("Last event: none"));
    rig.rec.click(rig.widget("Button", "label", "Press me"));
    rig.settle();
    CHECK(rig.shows("Last event: pressed"));
}

TEST_CASE("gallery: the radio choice switches the Switch's content", "[gallery][view]") {
    Rig rig;
    rig.settle();
    CHECK(rig.shows("a circle"));
    auto const selects = rig.rec.all("Select");
    REQUIRE(selects.size() == 2);  // created in view order: the colour dropdown, then the shape radio
    rig.rec.choose(selects.at(1), morph::ui::Key{std::int64_t{1}});
    rig.settle();
    CHECK(rig.shows("a square"));
    CHECK_FALSE(rig.shows("a circle"));
}

TEST_CASE("gallery: the keyed list adds, removes and keeps the rows it keeps", "[gallery][view]") {
    Rig rig;
    rig.settle();
    auto const banana = rig.widget("Text", "text", "banana");
    rig.rec.click(rig.widget("Button", "label", "Add"));
    rig.settle();
    CHECK(rig.shows("fruit 4"));
    rig.rec.click(rig.widget("Button", "label", "Remove first"));
    rig.settle();
    CHECK_FALSE(rig.shows("apple"));
    CHECK(rig.rec.exists(banana));
    CHECK(rig.widget("Text", "text", "banana") == banana);
}

TEST_CASE("gallery: dragging a shelf item onto the basket moves it", "[gallery][view]") {
    Rig rig;
    rig.settle();
    auto const basket = rig.widget("Panel", "title", "Basket");
    CHECK(rig.rec.drag(rig.widget("Text", "text", "pear"), basket));
    rig.settle();
    CHECK(rig.shows("Last event: dropped pear in the basket"));
    // Now in the basket: no longer on the shelf, so not something the basket takes.
    CHECK_FALSE(rig.rec.drag(rig.widget("Text", "text", "pear"), basket));
}

TEST_CASE("gallery: the dialog opens, closes from its button and from a dismissal", "[gallery][view]") {
    Rig rig;
    rig.settle();
    auto const dialog = rig.widget("Dialog", "title", "A dialog");
    CHECK(rig.rec.prop(dialog, "open") == "false");
    CHECK_FALSE(rig.rec.find("Button", "label", "Close").has_value());  // the content mounts only while open
    rig.rec.click(rig.widget("Button", "label", "Open dialog"));
    rig.settle();
    CHECK(rig.rec.prop(dialog, "open") == "true");
    rig.rec.click(rig.widget("Button", "label", "Close"));
    rig.settle();
    CHECK(rig.rec.prop(dialog, "open") == "false");
    CHECK(rig.shows("Last event: dialog closed"));

    rig.rec.click(rig.widget("Button", "label", "Open dialog"));
    rig.settle();
    rig.rec.dismiss(dialog);
    rig.settle();
    CHECK(rig.shows("Last event: dialog dismissed"));
}

TEST_CASE("gallery: typing a name updates the greeting", "[gallery][view]") {
    Rig rig;
    rig.settle();
    CHECK(rig.shows("Hello, stranger"));
    rig.rec.edit(rig.widget("TextInput", "placeholder", "Your name"), "Ada");
    rig.settle();
    CHECK(rig.shows("Hello, Ada"));
}

TEST_CASE("gallery: Quit asks the frontend to quit with 0", "[gallery][view]") {
    Rig rig;
    rig.settle();
    rig.rec.click(rig.widget("Button", "label", "Quit"));
    CHECK(rig.ctx.quitCode() == std::optional<int>{0});
}
```

Create `examples/tui/gallery/tests/test_gallery_smoke.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/frontend.hpp>
#include <testkit/frontend_smoke.hpp>

#include "gallery_application.hpp"

using morph::examples::AppEnvironment;
using morph::examples::testing::runFrontendSmoke;
using morph::examples::testing::SmokeFrontend;

TEST_CASE("gallery: mounts and quits on the terminal UI", "[gallery][smoke]") {
    runFrontendSmoke(
        [](morph::ui::AppContext& ctx) { return gallery::client::makeApplication(ctx, AppEnvironment{}); },
        SmokeFrontend::Tui);
}

TEST_CASE("gallery: mounts and quits on Qt Quick", "[gallery][smoke]") {
    runFrontendSmoke(
        [](morph::ui::AppContext& ctx) { return gallery::client::makeApplication(ctx, AppEnvironment{}); },
        SmokeFrontend::QtQuick);
}
```

Create `examples/tui/gallery/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# The gallery: every view-tree node kind once, drag-and-drop included. gallery_app links morph and the
# toolkit-free morph_ladder_app_common only, so a toolkit include in app/ does not compile.

add_library(gallery_app STATIC app/gallery_application.cpp)
target_include_directories(gallery_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/app")
target_link_libraries(gallery_app PUBLIC morph::morph morph::ladder_app_common)
target_compile_features(gallery_app PUBLIC cxx_std_23)
apply_warnings(gallery_app)
if(AF_COVERAGE)
    apply_coverage(gallery_app)
endif()
if(DEFINED AF_SANITIZER)
    apply_sanitizers(gallery_app ${AF_SANITIZER})
endif()

morph_add_example_ui(TARGET gallery SOURCES ui/main.cpp LIBRARIES gallery_app)

if(MORPH_BUILD_TESTS)
    add_executable(gallery_tests tests/test_gallery_view.cpp tests/test_gallery_smoke.cpp)
    target_link_libraries(gallery_tests PRIVATE gallery_app morph::example_testkit morph_test_main)
    target_compile_features(gallery_tests PRIVATE cxx_std_23)
    apply_warnings(gallery_tests)
    if(AF_COVERAGE)
        apply_coverage(gallery_tests)
    endif()
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(gallery_tests ${AF_SANITIZER})
    endif()
    include(Catch)
    catch_discover_tests(gallery_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS examples-tui TIMEOUT 120)
endif()
```

Create `examples/tui/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# Two example applications with no ladder rung, each an application library, its one binary and its tests:
# the gallery (every view-tree node kind, for frontend authors) and the workout dashboard (a Store-driven
# application with a timer). Added from the root when MORPH_BUILD_EXAMPLES and a frontend are on.

add_subdirectory(gallery)
```

Append to the root's "Example applications on an injected frontend" block, after its `endif()`:

```cmake
# examples/tui: two applications with no ladder rung. Natively only: a browser build of them would add a
# WebAssembly target nothing deploys.
if(MORPH_BUILD_EXAMPLES AND NOT EMSCRIPTEN AND (MORPH_BUILD_TUI OR MORPH_BUILD_QT_QUICK))
    add_subdirectory(examples/tui)
endif()
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake -S . -B build/all && cmake --build build/all --target gallery_tests`
Expected: FAIL — `Cannot find source file: app/gallery_application.cpp`.

- [ ] **Step 3: Implement**

Create `examples/tui/gallery/app/gallery_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/app_environment.hpp>
#include <memory>
#include <morph/ui/frontend.hpp>

/// @file
/// @brief The gallery: every view-tree node kind once, drag-and-drop included — a visual check for a frontend.

namespace gallery::client {

/// @brief Builds the gallery application.
/// @param ctx The frontend's context; its quit ends the gallery.
/// @param env The deployment choices; the gallery reads none of them.
/// @return The application.
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env);

}  // namespace gallery::client
```

Create `examples/tui/gallery/app/gallery_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "gallery_application.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/reactive/signal.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace gallery::client {

namespace {

namespace ui = morph::ui;
using morph::reactive::Signal;
using ui::Node;

enum class Shape : std::uint8_t { Circle, Square, Triangle };

struct Fruit {
    std::int64_t id = 0;
    std::string name;
    bool operator==(Fruit const&) const = default;
};

std::string keyText(ui::Key const& key) {
    if (auto const* number = std::get_if<std::int64_t>(&key)) {
        return std::to_string(*number);
    }
    return std::get<std::string>(key);
}

ui::Key fruitKey(Fruit const& fruit) {
    return ui::Key{fruit.id};
}

// A row's name. The row signal lives as long as the row's mount, which owns this binding.
Node fruitName(Signal<Fruit> const& row) {
    return ui::text({.text = [row = &row] { return row->get().name; }});
}

ui::Node stretch() {
    return ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}});
}

class GalleryApplication final : public ui::Application {
public:
    explicit GalleryApplication(ui::AppContext& ctx)
        : _ctx{&ctx},
          _lastEvent{ctx.runtime(), std::string{"Last event: none"}},
          _subscribed{ctx.runtime(), false},
          _busy{ctx.runtime(), false},
          _colour{ctx.runtime(), std::optional<ui::Key>{ui::Key{std::string{"green"}}}},
          _shape{ctx.runtime(), Shape::Circle},
          _name{ctx.runtime(), std::string{}},
          _notes{ctx.runtime(), std::string{}},
          _secret{ctx.runtime(), std::string{}},
          _when{ctx.runtime(), std::optional<morph::time::Timestamp>{}},
          _volume{ctx.runtime(), std::int64_t{50}},
          _file{ctx.runtime(), std::string{}},
          _detailsCollapsed{ctx.runtime(), true},
          _tab{ctx.runtime(), std::size_t{0}},
          _dialogOpen{ctx.runtime(), false},
          _fruits{ctx.runtime(), std::vector<Fruit>{Fruit{.id = 1, .name = "apple"}, Fruit{.id = 2, .name = "banana"},
                                                    Fruit{.id = 3, .name = "cherry"}}},
          _selection{ctx.runtime(), std::vector<ui::Key>{}},
          _shelf{ctx.runtime(), std::vector<Fruit>{Fruit{.id = 10, .name = "pear"}, Fruit{.id = 11, .name = "plum"}}},
          _basket{ctx.runtime(), std::vector<Fruit>{}} {}

    [[nodiscard]] Node view() override {
        return ui::scroll({.child = ui::column({.children = {header(), controls(), choices(), containers(),
                                                             collections(), inputs(), dragAndDrop(), overlays()},
                                                .gap = 1})});
    }

private:
    void note(std::string const& what) { _lastEvent.set("Last event: " + what); }

    Node header() {
        return ui::row({.children = {ui::text({.text = "morph UI gallery", .role = ui::TextRole::Heading}), stretch(),
                                     ui::text({.text = [this] { return _lastEvent.get(); }, .role = ui::TextRole::Muted}),
                                     ui::button({.label = "Quit", .onClick = [this] { _ctx->quit(0); }})},
                        .gap = 2});
    }

    Node controls() {
        auto buttons = ui::row(
            {.children = {ui::button({.label = "Press me", .onClick = [this] { note("pressed"); }}),
                          ui::checkbox({.label = "Subscribed",
                                        .checked = [this] { return _subscribed.get(); },
                                        .onToggle =
                                            [this](bool subscribed) {
                                                _subscribed.set(subscribed);
                                                note(subscribed ? "subscribed" : "unsubscribed");
                                            }}),
                          ui::button({.label = "Toggle busy", .onClick = [this] { _busy.set(!_busy.peek()); }}),
                          ui::busy({.active = [this] { return _busy.get(); }, .label = "Working"})},
             .gap = 2});
        auto roles = ui::row({.children = {ui::text({.text = "normal"}),
                                           ui::text({.text = "muted", .role = ui::TextRole::Muted}),
                                           ui::text({.text = "heading", .role = ui::TextRole::Heading}),
                                           ui::text({.text = "error", .role = ui::TextRole::Error}),
                                           ui::text({.text = "success", .role = ui::TextRole::Success})},
                              .gap = 2});
        return ui::panel({.title = "Controls", .padding = 1, .child = ui::column({.children = {buttons, roles}})});
    }

    Node choices() {
        std::vector<ui::SelectOption> const colours{{.key = ui::Key{std::string{"red"}}, .label = "Red"},
                                                    {.key = ui::Key{std::string{"green"}}, .label = "Green"},
                                                    {.key = ui::Key{std::string{"blue"}}, .label = "Blue"}};
        std::vector<ui::SelectOption> const shapes{{.key = ui::Key{std::int64_t{0}}, .label = "Circle"},
                                                   {.key = ui::Key{std::int64_t{1}}, .label = "Square"},
                                                   {.key = ui::Key{std::int64_t{2}}, .label = "Triangle"}};
        auto colour = ui::select({.options = colours,
                                  .selected = [this] { return _colour.get(); },
                                  .onSelect =
                                      [this](ui::Key key) {
                                          note("colour " + keyText(key));
                                          _colour.set(std::move(key));
                                      },
                                  .style = ui::SelectStyle::Dropdown});
        auto shape = ui::select(
            {.options = shapes,
             .selected = [this] { return std::optional<ui::Key>{ui::Key{static_cast<std::int64_t>(_shape.get())}}; },
             .onSelect =
                 [this](ui::Key const& key) {
                     auto const* index = std::get_if<std::int64_t>(&key);
                     if (index != nullptr && *index >= 0 && *index <= 2) {
                         _shape.set(static_cast<Shape>(*index));
                     }
                 },
             .style = ui::SelectStyle::Radio});
        auto described = ui::switchOn<Shape>([this] { return _shape.get(); },
                                             {{Shape::Circle, ui::text({.text = "a circle"})},
                                              {Shape::Square, ui::text({.text = "a square"})},
                                              {Shape::Triangle, ui::text({.text = "a triangle"})}});
        auto actions = ui::menu({.items = {{.label = "Say hello", .onSelect = [this] { note("hello"); }},
                                           {.label = "Say goodbye", .onSelect = [this] { note("goodbye"); }}}});
        return ui::panel({.title = "Choices",
                          .padding = 1,
                          .child = ui::column({.children = {colour, shape, described, actions}})});
    }

    Node containers() {
        std::vector<ui::GridCell> cells;
        for (int i = 1; i <= 5; ++i) {
            cells.push_back(ui::GridCell{.node = ui::text({.text = "cell " + std::to_string(i)}), .span = i == 5 ? 2 : 1});
        }
        auto details = ui::panel({.title = "Details",
                                  .padding = 1,
                                  .child = ui::text({.text = "Collapsible content"}),
                                  .collapsible = true,
                                  .collapsed = [this] { return _detailsCollapsed.get(); },
                                  .onToggle =
                                      [this](bool collapsed) {
                                          _detailsCollapsed.set(collapsed);
                                          note(collapsed ? "details collapsed" : "details expanded");
                                      }});
        return ui::panel(
            {.title = "Containers",
             .padding = 1,
             .child = ui::column(
                 {.children = {ui::grid({.columns = 3, .cells = std::move(cells), .gap = 1}),
                               ui::row({.children = {ui::text({.text = "left"}), stretch(), ui::text({.text = "right"})}}),
                               details}})});
    }

    Node collections() {
        auto buttons = ui::row({.children = {ui::button({.label = "Add", .onClick = [this] { addFruit(); }}),
                                             ui::button({.label = "Remove first", .onClick = [this] { removeFirst(); }}),
                                             ui::button({.label = "Reverse", .onClick = [this] { reverse(); }})},
                                .gap = 1});
        auto list = ui::forEach<Fruit>(_fruits, fruitKey, fruitName);
        auto table = ui::table<Fruit>(
            {{.label = "Id", .width = ui::Sizing::fixed(4)}, {.label = "Name", .width = ui::Sizing::stretch()}},
            [this] { return _fruits.get(); }, fruitKey,
            [](Signal<Fruit> const& row) {
                return std::vector<Node>{ui::text({.text = [row = &row] { return std::to_string(row->get().id); }}),
                                         fruitName(row)};
            },
            ui::TableOptions{.selectionMode = ui::SelectionMode::Multiple,
                             .selection = [this] { return _selection.get(); },
                             .onSelectionChange =
                                 [this](std::vector<ui::Key> keys) {
                                     note(std::to_string(keys.size()) + " selected");
                                     _selection.set(std::move(keys));
                                 },
                             .onActivate = [this](ui::Key const& key) { note("activated " + keyText(key)); }});
        return ui::panel({.title = "Collections", .padding = 1, .child = ui::column({.children = {buttons, list, table}})});
    }

    Node inputs() {
        auto name = ui::textInput({.value = [this] { return _name.get(); },
                                   .onChange = [this](std::string text) { _name.set(std::move(text)); },
                                   .onSubmit = [this](std::string const& text) { note("submitted " + text); },
                                   .placeholder = "Your name"});
        auto greeting = ui::text({.text = [this] {
            return _name.get().empty() ? std::string{"Hello, stranger"} : "Hello, " + _name.get();
        }});
        auto notes = ui::textInput({.value = [this] { return _notes.get(); },
                                    .onChange = [this](std::string text) { _notes.set(std::move(text)); },
                                    .placeholder = "Notes",
                                    .mode = ui::TextInputMode::Multiline});
        auto secret = ui::textInput({.value = [this] { return _secret.get(); },
                                     .onChange = [this](std::string text) { _secret.set(std::move(text)); },
                                     .placeholder = "Password",
                                     .mode = ui::TextInputMode::Password});
        auto when = ui::dateTimeInput({.value = [this] { return _when.get(); },
                                       .onChange =
                                           [this](std::optional<morph::time::Timestamp> value) {
                                               _when.set(value);
                                               note("date changed");
                                           },
                                       .mode = ui::DateMode::DateTime});
        auto volume = ui::row(
            {.children = {ui::slider({.value = [this] { return _volume.get(); },
                                      .minimum = 0,
                                      .maximum = 100,
                                      .step = 5,
                                      .onChange = [this](std::int64_t value) { _volume.set(value); }}),
                          ui::text({.text = [this] { return "Volume " + std::to_string(_volume.get()); }})},
             .gap = 1});
        auto file = ui::filePicker({.path = [this] { return _file.get(); },
                                    .mode = ui::FilePickerMode::Open,
                                    .onPicked =
                                        [this](std::string path) {
                                            note("picked " + path);
                                            _file.set(std::move(path));
                                        }});
        return ui::panel({.title = "Inputs",
                          .padding = 1,
                          .child = ui::column({.children = {name, greeting, notes, secret, when, volume, file}})});
    }

    Node dragAndDrop() {
        auto const draggable = [](Signal<Fruit> const& row) {
            return ui::text({.text = [row = &row] { return row->get().name; },
                             .common = {.dragKey = [row = &row] { return std::optional<ui::Key>{fruitKey(row->get())}; }}});
        };
        auto shelf = ui::panel({.title = "Shelf", .padding = 1, .child = ui::forEach<Fruit>(_shelf, fruitKey, draggable)});
        auto basket = ui::panel({.title = "Basket",
                                 .padding = 1,
                                 .child = ui::forEach<Fruit>(_basket, fruitKey, fruitName),
                                 .common = {.accepts = [this](ui::Key const& key) { return onShelf(key); },
                                            .onDrop = [this](ui::Key const& key) { moveToBasket(key); }}});
        return ui::panel({.title = "Drag and drop", .padding = 1, .child = ui::row({.children = {shelf, basket}, .gap = 2})});
    }

    Node overlays() {
        auto pages = ui::tabs({.tabs = {{.label = "First", .node = ui::text({.text = "The first tab"})},
                                        {.label = "Second", .node = ui::text({.text = "The second tab"})}},
                               .selected = [this] { return _tab.get(); },
                               .onSelect =
                                   [this](std::size_t tab) {
                                       _tab.set(tab);
                                       note("tab " + std::to_string(tab));
                                   }});
        auto open = ui::button({.label = "Open dialog", .onClick = [this] {
                                    _dialogOpen.set(true);
                                    note("dialog opened");
                                }});
        auto dialog = ui::dialog(
            {.open = [this] { return _dialogOpen.get(); },
             .title = "A dialog",
             .child = ui::column({.children = {ui::text({.text = "Focus stays here until the dialog closes."}),
                                               ui::button({.label = "Close", .onClick = [this] {
                                                               _dialogOpen.set(false);
                                                               note("dialog closed");
                                                           }})}}),
             .onDismiss = [this] {
                 _dialogOpen.set(false);
                 note("dialog dismissed");
             }});
        return ui::panel({.title = "Overlays", .padding = 1, .child = ui::column({.children = {pages, open, dialog}})});
    }

    void addFruit() {
        auto const fruitId = _nextFruit++;
        _fruits.mutate([fruitId](std::vector<Fruit>& fruits) {
            fruits.push_back(Fruit{.id = fruitId, .name = "fruit " + std::to_string(fruitId)});
        });
        note("added fruit " + std::to_string(fruitId));
    }

    void removeFirst() {
        if (_fruits.peek().empty()) {
            return;
        }
        _fruits.mutate([](std::vector<Fruit>& fruits) { fruits.erase(fruits.begin()); });
        note("removed the first fruit");
    }

    void reverse() {
        _fruits.mutate([](std::vector<Fruit>& fruits) { std::ranges::reverse(fruits); });
        note("reversed the fruit");
    }

    [[nodiscard]] bool onShelf(ui::Key const& key) const {
        return std::ranges::any_of(_shelf.peek(), [&key](Fruit const& fruit) { return fruitKey(fruit) == key; });
    }

    void moveToBasket(ui::Key const& key) {
        auto shelf = _shelf.peek();
        auto const found = std::ranges::find_if(shelf, [&key](Fruit const& fruit) { return fruitKey(fruit) == key; });
        if (found == shelf.end()) {
            return;
        }
        Fruit const moved = *found;
        shelf.erase(found);
        _shelf.set(std::move(shelf));
        _basket.mutate([&moved](std::vector<Fruit>& basket) { basket.push_back(moved); });
        note("dropped " + moved.name + " in the basket");
    }

    ui::AppContext* _ctx;
    Signal<std::string> _lastEvent;
    Signal<bool> _subscribed;
    Signal<bool> _busy;
    Signal<std::optional<ui::Key>> _colour;
    Signal<Shape> _shape;
    Signal<std::string> _name;
    Signal<std::string> _notes;
    Signal<std::string> _secret;
    Signal<std::optional<morph::time::Timestamp>> _when;
    Signal<std::int64_t> _volume;
    Signal<std::string> _file;
    Signal<bool> _detailsCollapsed;
    Signal<std::size_t> _tab;
    Signal<bool> _dialogOpen;
    Signal<std::vector<Fruit>> _fruits;
    Signal<std::vector<ui::Key>> _selection;
    Signal<std::vector<Fruit>> _shelf;
    Signal<std::vector<Fruit>> _basket;
    std::int64_t _nextFruit = 4;
};

}  // namespace

std::unique_ptr<ui::Application> makeApplication(ui::AppContext& ctx,
                                                 morph::examples::AppEnvironment const& /*env*/) {
    return std::make_unique<GalleryApplication>(ctx);
}

}  // namespace gallery::client
```

Create `examples/tui/gallery/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <vector>

#include "gallery_application.hpp"

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
        return frontend->run(
            [&env](morph::ui::AppContext& ctx) { return gallery::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "gallery: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "gallery: unknown error\n";
    }
    return 1;
}
```

Create `examples/tui/README.md`:

```markdown
# examples/tui — the gallery and the workout dashboard

Two applications with no ladder rung. Each is an application library (`app/`, toolkit-free), one binary
(`ui/main.cpp`) that picks its frontend at runtime, and tests (`tests/`): view tests on the headless
`RecordingBackend` and a smoke test per built frontend.

| Binary | What it shows |
|---|---|
| `gallery` | Every view-tree node kind once — text roles, buttons, inputs, selects, a switch, a menu, a grid, panels, a keyed list, a table, tabs, a dialog, a busy indicator, date, slider and file inputs — and drag-and-drop from a shelf onto a basket. A visual check for anyone writing a frontend. |

Build with `-DMORPH_BUILD_TUI=ON` and/or `-DMORPH_BUILD_QT_QUICK=ON` (examples are on by default). The binary
chooses with `--ui=tui` or `--ui=qt`, else `MORPH_UI`, else the first usable frontend (Qt Quick when a display
is available, the terminal when stdin is one):

    ./build/examples/tui/gallery/gallery --ui=tui
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/all --target gallery gallery_tests && ./build/all/examples/tui/gallery/gallery_tests`
Expected: PASS, 10 test cases; then `cmake --build build/ex-tui --target gallery gallery_tests` and
`build/ex-qt` likewise build, and their `gallery_tests` pass with the other frontend's smoke case skipped.
By hand once: `./build/all/examples/tui/gallery/gallery --ui=tui` shows the gallery; Tab moves focus, a drag from
"pear" onto "Basket" moves it, the Quit button exits.
Mutation check: delete `.common = {.dragKey = …}` from `draggable`. Expected FAIL in "dragging a shelf item onto
the basket moves it" (`drag` returns false). Restore. Toolkit check: add `#include <QObject>` at the top of
`gallery_application.cpp` and build `gallery_app` in `build/all`. Expected: compile error `'QObject' file not
found` — the library's link set names no toolkit. Remove the line.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt examples/tui
git commit -m "wip(examples-common): examples/tui/gallery, every node kind on the injected frontend

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: `examples/tui/workout` — a Store-driven dashboard with a ticker

Spec 1 §8: a dashboard with a `Store` of signal fields and Msgs, `Tabs` (Dashboard / Statistics), a `Menu`
table, a keyed lap `forEach`, a Sprint `Button` with a bound `enabled`, a rider-name `TextInput`, a Quit
confirmation `Dialog`, and a `Scheduler`-driven ticker. It is shaped like the migrated apps: a controller (Store,
`Computed` projections, the ticker's handle last) tested headless, a view of bindings only, and an application
owning the controller.

**Files:**
- Create: `examples/tui/workout/CMakeLists.txt`, `app/workout_controller.{hpp,cpp}`, `app/workout_view.{hpp,cpp}`,
  `app/workout_application.{hpp,cpp}`, `ui/main.cpp`
- Create: `examples/tui/workout/tests/.clang-tidy` (Task 2's content), `tests/test_workout_controller.cpp`,
  `tests/test_workout_view.cpp`, `tests/test_workout_smoke.cpp`
- Modify: `examples/tui/CMakeLists.txt` — `add_subdirectory(workout)` after `add_subdirectory(gallery)`
- Modify: `examples/tui/README.md` — the workout row

**Interfaces:**
- Consumes: `reactive::Store<ViewState, Msg>(Runtime&, Init, Update)`, `Store::{send, action, state}`,
  `reactive::Computed<T>(Runtime&, F)`, `reactive::Signal::{get, peek, set, mutate}`, `reactive::Scheduler::every`,
  `reactive::TimerHandle` (Part 1); the Part 2 builders, `Mounted`, `RecordingBackend::{find, all, prop, click,
  edit, chooseIndex}`; `examples::AppEnvironment::fromArgs`; `tui::frontendOption`, `qt_quick::frontendOption` (as
  Task 10); `FakeAppContext`, `runFrontendSmoke`; `morph_add_example_ui`.
- Produces: `workout::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&)` (the workout reads
  nothing from the environment); `workout::client::WorkoutController`, `WorkoutState`, `WorkoutMsg`, `Lap`,
  `formatDuration`, `lapLine`, `kSprintCooldownSeconds`, `kTickPeriod`; targets `workout_app`, `workout`,
  `workout_tests` (label `examples-tui`).

- [ ] **Step 1: Write the failing tests**

Create `examples/tui/workout/tests/test_workout_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <testkit/fake_app_context.hpp>
#include <vector>

#include "workout_controller.hpp"

namespace {

using morph::examples::testing::FakeAppContext;
using namespace workout::client;

struct Rig {
    FakeAppContext ctx;
    std::unique_ptr<WorkoutController> controller =
        std::make_unique<WorkoutController>(ctx.runtime(), ctx.manualScheduler());

    void ride(int seconds) { ctx.manualScheduler().advance(std::chrono::seconds{seconds}); }
};

}  // namespace

TEST_CASE("workout: durations read mm:ss, and h:mm:ss past an hour", "[workout][controller]") {
    CHECK(formatDuration(0) == "00:00");
    CHECK(formatDuration(65) == "01:05");
    CHECK(formatDuration(3600) == "1:00:00");
    CHECK(formatDuration(3725) == "1:02:05");
    CHECK(formatDuration(-5) == "00:00");
    CHECK(lapLine(Lap{.number = 1, .seconds = 62, .sprint = false}) == "Lap 1  01:02");
    CHECK(lapLine(Lap{.number = 2, .seconds = 40, .sprint = true}) == "Lap 2  00:40  sprint");
}

TEST_CASE("workout: the ticker counts only while riding", "[workout][controller]") {
    Rig rig;
    rig.ride(3);
    CHECK(rig.controller->elapsedText() == "00:00");
    rig.controller->send(Start{});
    rig.ride(3);
    CHECK(rig.controller->elapsedText() == "00:03");
    CHECK(rig.controller->lapText() == "00:03");
    rig.controller->send(Pause{});
    rig.ride(5);
    CHECK(rig.controller->elapsedText() == "00:03");
}

TEST_CASE("workout: Sprint needs a ride under way, a lap started and no cooldown", "[workout][controller]") {
    Rig rig;
    CHECK_FALSE(rig.controller->canSprint());
    rig.controller->send(Start{});
    CHECK_FALSE(rig.controller->canSprint());  // nothing ridden in this lap yet
    rig.ride(1);
    CHECK(rig.controller->canSprint());
    CHECK(rig.controller->sprintLabel() == "Sprint");

    rig.controller->send(Sprint{});
    REQUIRE(rig.controller->state().laps.peek().size() == 1);
    CHECK(rig.controller->state().laps.peek().front().sprint);
    CHECK_FALSE(rig.controller->canSprint());
    CHECK(rig.controller->sprintLabel() == "Sprint (30s)");

    rig.ride(kSprintCooldownSeconds);
    CHECK(rig.controller->canSprint());
    CHECK(rig.controller->sprintLabel() == "Sprint");
}

TEST_CASE("workout: a lap closes the current one and the statistics follow", "[workout][controller]") {
    Rig rig;
    CHECK(rig.controller->bestLapText() == "-");
    CHECK(rig.controller->averageLapText() == "-");
    rig.controller->send(Start{});
    rig.ride(10);
    rig.controller->send(MarkLap{});
    rig.ride(20);
    rig.controller->send(MarkLap{});
    CHECK(rig.controller->lapCountText() == "2");
    CHECK(rig.controller->bestLapText() == "00:10");
    CHECK(rig.controller->averageLapText() == "00:15");
    CHECK(rig.controller->sprintCountText() == "0");
    CHECK(rig.controller->lapText() == "00:00");
}

TEST_CASE("workout: a lap with nothing ridden is not recorded", "[workout][controller]") {
    Rig rig;
    rig.controller->send(Start{});
    rig.controller->send(MarkLap{});
    CHECK(rig.controller->state().laps.peek().empty());
}

TEST_CASE("workout: Reset clears the ride", "[workout][controller]") {
    Rig rig;
    rig.controller->send(Start{});
    rig.ride(12);
    rig.controller->send(MarkLap{});
    rig.controller->send(Reset{});
    CHECK(rig.controller->elapsedText() == "00:00");
    CHECK(rig.controller->lapCountText() == "0");
    CHECK(rig.controller->statusText() == "Ready");
}

TEST_CASE("workout: the status says what the rider is doing", "[workout][controller]") {
    Rig rig;
    CHECK(rig.controller->statusText() == "Ready");
    rig.controller->send(Rename{.rider = "Ada"});
    CHECK(rig.controller->statusText() == "Ready, Ada");
    rig.controller->send(Start{});
    CHECK(rig.controller->statusText() == "Riding, Ada");
    rig.ride(1);
    rig.controller->send(Pause{});
    CHECK(rig.controller->statusText() == "Paused, Ada");
}

TEST_CASE("workout: destroying the controller stops its ticker", "[workout][controller]") {
    Rig rig;
    CHECK(rig.ctx.manualScheduler().pendingTimers() == 1);
    rig.controller.reset();
    CHECK(rig.ctx.manualScheduler().pendingTimers() == 0);
}
```

Create `examples/tui/workout/tests/test_workout_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string_view>
#include <testkit/fake_app_context.hpp>

#include "workout_application.hpp"

namespace {

using morph::examples::AppEnvironment;
using morph::examples::testing::FakeAppContext;
using morph::ui::testing::RecordingBackend;
using namespace std::chrono_literals;

constexpr std::size_t kMenuStart = 0;
constexpr std::size_t kMenuQuit = 4;

struct Rig {
    FakeAppContext ctx;
    std::unique_ptr<morph::ui::Application> app = workout::client::makeApplication(ctx, AppEnvironment{});
    RecordingBackend rec;
    morph::ui::Mounted mounted{ctx.runtime(), rec, app->view()};

    void settle() { ctx.owner().drain(); }

    int widget(std::string_view kind, std::string_view prop, std::string_view value) {
        auto const found = rec.find(kind, prop, value);
        REQUIRE(found.has_value());
        return *found;
    }

    int only(std::string_view kind) {
        auto const ids = rec.all(kind);
        REQUIRE(ids.size() == 1);
        return ids.front();
    }

    [[nodiscard]] bool shows(std::string_view text) const { return rec.find("Text", "text", text).has_value(); }
};

}  // namespace

TEST_CASE("workout: the view opens on the dashboard with Sprint disabled and the dialog closed", "[workout][view]") {
    Rig rig;
    rig.settle();
    static_cast<void>(rig.only("Menu"));
    static_cast<void>(rig.only("Tabs"));
    CHECK(rig.rec.prop(rig.widget("Button", "label", "Sprint"), "enabled") == "false");
    CHECK(rig.rec.prop(rig.only("Dialog"), "open") == "false");
    CHECK(rig.shows("Ready"));
}

TEST_CASE("workout: Start from the menu, a tick and Sprint drive the dashboard", "[workout][view]") {
    Rig rig;
    rig.settle();
    auto const sprint = rig.widget("Button", "label", "Sprint");
    rig.rec.chooseIndex(rig.only("Menu"), kMenuStart);
    rig.settle();
    rig.ctx.manualScheduler().advance(1000ms);
    rig.settle();
    CHECK(rig.rec.prop(sprint, "enabled") == "true");
    rig.rec.click(sprint);
    rig.settle();
    CHECK(rig.shows("Lap 1  00:01  sprint"));
    CHECK(rig.rec.prop(sprint, "label") == "Sprint (30s)");
    CHECK(rig.rec.prop(sprint, "enabled") == "false");
}

TEST_CASE("workout: typing the rider's name shows in the status", "[workout][view]") {
    Rig rig;
    rig.settle();
    rig.rec.edit(rig.widget("TextInput", "placeholder", "Rider name"), "Ada");
    rig.settle();
    CHECK(rig.shows("Ready, Ada"));
}

TEST_CASE("workout: Quit asks first; Keep riding cancels; Quit in the dialog exits 0", "[workout][view]") {
    Rig rig;
    rig.settle();
    auto const menu = rig.only("Menu");
    auto const dialog = rig.only("Dialog");
    rig.rec.chooseIndex(menu, kMenuQuit);
    rig.settle();
    CHECK(rig.rec.prop(dialog, "open") == "true");
    rig.rec.click(rig.widget("Button", "label", "Keep riding"));
    rig.settle();
    CHECK(rig.rec.prop(dialog, "open") == "false");
    CHECK_FALSE(rig.ctx.quitCode().has_value());

    rig.rec.chooseIndex(menu, kMenuQuit);
    rig.settle();
    rig.rec.click(rig.widget("Button", "label", "Quit"));
    CHECK(rig.ctx.quitCode() == std::optional<int>{0});
}

TEST_CASE("workout: the Statistics tab shows the lap figures", "[workout][view]") {
    Rig rig;
    rig.settle();
    rig.rec.chooseIndex(rig.only("Menu"), kMenuStart);
    rig.settle();
    rig.ctx.manualScheduler().advance(10000ms);
    rig.rec.click(rig.widget("Button", "label", "Lap"));
    rig.settle();
    CHECK_FALSE(rig.shows("Best lap"));  // tabs mount lazily
    rig.rec.chooseIndex(rig.only("Tabs"), 1);
    rig.settle();
    CHECK(rig.shows("Best lap"));
    CHECK(rig.shows("1"));  // laps ridden: only the Statistics tab shows a bare count
}
```

Create `examples/tui/workout/tests/test_workout_smoke.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/frontend.hpp>
#include <testkit/frontend_smoke.hpp>

#include "workout_application.hpp"

using morph::examples::AppEnvironment;
using morph::examples::testing::runFrontendSmoke;
using morph::examples::testing::SmokeFrontend;

TEST_CASE("workout: mounts and quits on the terminal UI", "[workout][smoke]") {
    runFrontendSmoke(
        [](morph::ui::AppContext& ctx) { return workout::client::makeApplication(ctx, AppEnvironment{}); },
        SmokeFrontend::Tui);
}

TEST_CASE("workout: mounts and quits on Qt Quick", "[workout][smoke]") {
    runFrontendSmoke(
        [](morph::ui::AppContext& ctx) { return workout::client::makeApplication(ctx, AppEnvironment{}); },
        SmokeFrontend::QtQuick);
}
```

Create `examples/tui/workout/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# The workout dashboard: a Store-driven controller with a scheduler ticker, its view, and its one binary.
# workout_app links morph and the toolkit-free morph_ladder_app_common only, so a toolkit include in app/ does
# not compile.

add_library(workout_app STATIC
    app/workout_controller.cpp
    app/workout_view.cpp
    app/workout_application.cpp)
target_include_directories(workout_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/app")
target_link_libraries(workout_app PUBLIC morph::morph morph::ladder_app_common)
target_compile_features(workout_app PUBLIC cxx_std_23)
apply_warnings(workout_app)
if(AF_COVERAGE)
    apply_coverage(workout_app)
endif()
if(DEFINED AF_SANITIZER)
    apply_sanitizers(workout_app ${AF_SANITIZER})
endif()

morph_add_example_ui(TARGET workout SOURCES ui/main.cpp LIBRARIES workout_app)

if(MORPH_BUILD_TESTS)
    add_executable(workout_tests
        tests/test_workout_controller.cpp
        tests/test_workout_view.cpp
        tests/test_workout_smoke.cpp)
    target_link_libraries(workout_tests PRIVATE workout_app morph::example_testkit morph_test_main)
    target_compile_features(workout_tests PRIVATE cxx_std_23)
    apply_warnings(workout_tests)
    if(AF_COVERAGE)
        apply_coverage(workout_tests)
    endif()
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(workout_tests ${AF_SANITIZER})
    endif()
    include(Catch)
    catch_discover_tests(workout_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS examples-tui TIMEOUT 120)
endif()
```

Add `add_subdirectory(workout)` after `add_subdirectory(gallery)` in `examples/tui/CMakeLists.txt`.

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake -S . -B build/all && cmake --build build/all --target workout_tests`
Expected: FAIL — `Cannot find source file: app/workout_controller.cpp`.

- [ ] **Step 3: Implement**

Create `examples/tui/workout/app/workout_controller.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief The workout dashboard's controller: a `Store` of the ride, the projections its view binds to, and the
///        one-second ticker.

namespace workout::client {

/// @brief How long Sprint stays unavailable after a sprint.
inline constexpr std::int64_t kSprintCooldownSeconds = 30;
/// @brief The ticker's period: one tick is one ridden second.
inline constexpr std::chrono::milliseconds kTickPeriod{1000};

/// @brief One completed lap.
struct Lap {
    std::int64_t number = 0;   ///< 1-based, in order of completion; the lap list's key.
    std::int64_t seconds = 0;  ///< Its duration.
    bool sprint = false;       ///< Closed by Sprint rather than Lap.
    bool operator==(Lap const&) const = default;
};

/// @brief The ride: what the user did and what the ticker counted.
struct WorkoutState {
    morph::reactive::Signal<std::string> rider;          ///< The rider's name; may be empty.
    morph::reactive::Signal<bool> running;               ///< Whether the ticker counts.
    morph::reactive::Signal<std::int64_t> elapsed;       ///< Seconds ridden in total.
    morph::reactive::Signal<std::int64_t> lapElapsed;    ///< Seconds ridden in the current lap.
    morph::reactive::Signal<std::int64_t> cooldown;      ///< Seconds until Sprint is available again.
    morph::reactive::Signal<std::vector<Lap>> laps;      ///< Completed laps, oldest first.
    morph::reactive::Signal<std::size_t> tab;            ///< The shown tab: 0 Dashboard, 1 Statistics.
    morph::reactive::Signal<bool> confirmingQuit;        ///< Whether the quit confirmation is open.
};

struct Start {};       ///< Start or resume riding.
struct Pause {};       ///< Stop the clock.
struct Tick {};        ///< One second passed (the ticker).
struct MarkLap {};     ///< Close the current lap.
struct Sprint {};      ///< Close the current lap as a sprint and start the cooldown.
struct Reset {};       ///< Clear the ride.
struct AskQuit {};     ///< Open the quit confirmation.
struct CancelQuit {};  ///< Close it.
/// @brief Rename the rider.
struct Rename {
    std::string rider;  ///< The new name.
};
/// @brief Show a tab.
struct ShowTab {
    std::size_t tab = 0;  ///< Its index.
};
/// @brief Everything the workout's view and ticker can ask of it.
using WorkoutMsg = std::variant<Start, Pause, Tick, MarkLap, Sprint, Reset, AskQuit, CancelQuit, Rename, ShowTab>;

/// @brief A duration as `mm:ss`, or `h:mm:ss` from an hour on; negative reads as zero.
/// @param seconds The duration.
/// @return The text.
[[nodiscard]] std::string formatDuration(std::int64_t seconds);

/// @brief One lap's line in the lap list: `Lap 2  00:40`, with `  sprint` appended for a sprint.
/// @param lap The lap.
/// @return The text.
[[nodiscard]] std::string lapLine(Lap const& lap);

/// @brief The ride's state, its updates, and the projections the view binds to.
///
/// Owns, in order: the store, the projections, and the ticker's handle, which goes first so no tick reaches a
/// half-destroyed store.
class WorkoutController {
public:
    /// @param runtime The runtime. Borrowed: it must outlive the controller.
    /// @param scheduler Runs the ticker. Borrowed: it must outlive the controller.
    WorkoutController(morph::reactive::Runtime& runtime, morph::reactive::Scheduler& scheduler);
    ~WorkoutController() = default;
    WorkoutController(WorkoutController const&) = delete;
    WorkoutController& operator=(WorkoutController const&) = delete;
    WorkoutController(WorkoutController&&) = delete;
    WorkoutController& operator=(WorkoutController&&) = delete;

    /// @brief Applies @p msg.
    /// @param msg The message.
    void send(WorkoutMsg msg) { _store.send(std::move(msg)); }

    /// @brief A callback that sends @p msg each time it runs — a button's or a menu item's action.
    /// @param msg The message.
    /// @return The callback.
    [[nodiscard]] std::function<void()> action(WorkoutMsg msg) { return _store.action(std::move(msg)); }

    /// @brief The ride.
    /// @return The state; its fields are tracked signals.
    [[nodiscard]] WorkoutState const& state() const { return _store.state(); }

    /// @brief `Ready`, `Riding` or `Paused`, then `, <rider>` when named. Tracked.
    /// @return The text.
    [[nodiscard]] std::string const& statusText() const { return _status.get(); }
    /// @brief Total ridden time. Tracked.
    /// @return The text.
    [[nodiscard]] std::string const& elapsedText() const { return _elapsed.get(); }
    /// @brief The current lap's time. Tracked.
    /// @return The text.
    [[nodiscard]] std::string const& lapText() const { return _lap.get(); }
    /// @brief `Sprint`, or `Sprint (Ns)` during the cooldown. Tracked.
    /// @return The text.
    [[nodiscard]] std::string const& sprintLabel() const { return _sprintLabel.get(); }
    /// @brief The number of completed laps. Tracked.
    /// @return The text.
    [[nodiscard]] std::string const& lapCountText() const { return _lapCount.get(); }
    /// @brief The fastest lap, or `-` before the first. Tracked.
    /// @return The text.
    [[nodiscard]] std::string const& bestLapText() const { return _bestLap.get(); }
    /// @brief The mean lap, or `-` before the first. Tracked.
    /// @return The text.
    [[nodiscard]] std::string const& averageLapText() const { return _averageLap.get(); }
    /// @brief The number of sprint laps. Tracked.
    /// @return The text.
    [[nodiscard]] std::string const& sprintCountText() const { return _sprintCount.get(); }
    /// @brief Whether Sprint does anything now: riding, a lap under way, no cooldown. Tracked.
    /// @return The answer.
    [[nodiscard]] bool canSprint() const { return _canSprint.get(); }

private:
    morph::reactive::Store<WorkoutState, WorkoutMsg> _store;
    morph::reactive::Computed<std::string> _status;
    morph::reactive::Computed<std::string> _elapsed;
    morph::reactive::Computed<std::string> _lap;
    morph::reactive::Computed<std::string> _sprintLabel;
    morph::reactive::Computed<std::string> _lapCount;
    morph::reactive::Computed<std::string> _bestLap;
    morph::reactive::Computed<std::string> _averageLap;
    morph::reactive::Computed<std::string> _sprintCount;
    morph::reactive::Computed<bool> _canSprint;
    morph::reactive::TimerHandle _ticker;
};

}  // namespace workout::client
```

Create `examples/tui/workout/app/workout_controller.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "workout_controller.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace workout::client {

namespace {

using morph::reactive::Runtime;

WorkoutState initialState(Runtime& runtime) {
    return WorkoutState{.rider{runtime, std::string{}},
                        .running{runtime, false},
                        .elapsed{runtime, std::int64_t{0}},
                        .lapElapsed{runtime, std::int64_t{0}},
                        .cooldown{runtime, std::int64_t{0}},
                        .laps{runtime, std::vector<Lap>{}},
                        .tab{runtime, std::size_t{0}},
                        .confirmingQuit{runtime, false}};
}

void closeLap(WorkoutState& state, bool sprint) {
    auto const seconds = state.lapElapsed.peek();
    if (seconds == 0) {
        return;
    }
    auto const number = static_cast<std::int64_t>(state.laps.peek().size()) + 1;
    state.laps.mutate([&](std::vector<Lap>& laps) {
        laps.push_back(Lap{.number = number, .seconds = seconds, .sprint = sprint});
    });
    state.lapElapsed.set(0);
}

// One handler per message; the Store refuses at compile time an update that misses one.
struct WorkoutUpdate {
    void operator()(WorkoutState& state, Start const& /*msg*/) const { state.running.set(true); }
    void operator()(WorkoutState& state, Pause const& /*msg*/) const { state.running.set(false); }

    void operator()(WorkoutState& state, Tick const& /*msg*/) const {
        if (!state.running.peek()) {
            return;
        }
        state.elapsed.set(state.elapsed.peek() + 1);
        state.lapElapsed.set(state.lapElapsed.peek() + 1);
        state.cooldown.set(std::max<std::int64_t>(0, state.cooldown.peek() - 1));
    }

    void operator()(WorkoutState& state, MarkLap const& /*msg*/) const { closeLap(state, false); }

    void operator()(WorkoutState& state, Sprint const& /*msg*/) const {
        if (!state.running.peek() || state.cooldown.peek() > 0 || state.lapElapsed.peek() == 0) {
            return;
        }
        closeLap(state, true);
        state.cooldown.set(kSprintCooldownSeconds);
    }

    void operator()(WorkoutState& state, Reset const& /*msg*/) const {
        state.running.set(false);
        state.elapsed.set(0);
        state.lapElapsed.set(0);
        state.cooldown.set(0);
        state.laps.set({});
    }

    void operator()(WorkoutState& state, AskQuit const& /*msg*/) const { state.confirmingQuit.set(true); }
    void operator()(WorkoutState& state, CancelQuit const& /*msg*/) const { state.confirmingQuit.set(false); }
    void operator()(WorkoutState& state, Rename const& msg) const { state.rider.set(msg.rider); }
    void operator()(WorkoutState& state, ShowTab const& msg) const { state.tab.set(msg.tab); }
};

}  // namespace

std::string formatDuration(std::int64_t seconds) {
    auto const total = std::max<std::int64_t>(0, seconds);
    auto const hours = total / 3600;
    auto const minutes = (total % 3600) / 60;
    auto const rest = total % 60;
    if (hours > 0) {
        return std::format("{}:{:02}:{:02}", hours, minutes, rest);
    }
    return std::format("{:02}:{:02}", minutes, rest);
}

std::string lapLine(Lap const& lap) {
    return std::format("Lap {}  {}{}", lap.number, formatDuration(lap.seconds), lap.sprint ? "  sprint" : "");
}

WorkoutController::WorkoutController(Runtime& runtime, morph::reactive::Scheduler& scheduler)
    : _store{runtime, initialState, WorkoutUpdate{}},
      _status{runtime,
              [this] {
                  auto const& state = _store.state();
                  std::string status = state.running.get() ? "Riding" : state.elapsed.get() > 0 ? "Paused" : "Ready";
                  if (auto const& rider = state.rider.get(); !rider.empty()) {
                      status += ", " + rider;
                  }
                  return status;
              }},
      _elapsed{runtime, [this] { return formatDuration(_store.state().elapsed.get()); }},
      _lap{runtime, [this] { return formatDuration(_store.state().lapElapsed.get()); }},
      _sprintLabel{runtime,
                   [this] {
                       auto const cooldown = _store.state().cooldown.get();
                       return cooldown > 0 ? std::format("Sprint ({}s)", cooldown) : std::string{"Sprint"};
                   }},
      _lapCount{runtime, [this] { return std::to_string(_store.state().laps.get().size()); }},
      _bestLap{runtime,
               [this] {
                   auto const& laps = _store.state().laps.get();
                   if (laps.empty()) {
                       return std::string{"-"};
                   }
                   return formatDuration(std::ranges::min(laps, {}, &Lap::seconds).seconds);
               }},
      _averageLap{runtime,
                  [this] {
                      auto const& laps = _store.state().laps.get();
                      if (laps.empty()) {
                          return std::string{"-"};
                      }
                      auto const total = std::accumulate(laps.begin(), laps.end(), std::int64_t{0},
                                                         [](std::int64_t sum, Lap const& lap) { return sum + lap.seconds; });
                      return formatDuration(total / static_cast<std::int64_t>(laps.size()));
                  }},
      _sprintCount{runtime,
                   [this] {
                       return std::to_string(std::ranges::count_if(_store.state().laps.get(), &Lap::sprint));
                   }},
      _canSprint{runtime,
                 [this] {
                     auto const& state = _store.state();
                     return state.running.get() && state.cooldown.get() == 0 && state.lapElapsed.get() > 0;
                 }},
      _ticker{scheduler.every(kTickPeriod, [this] { _store.send(Tick{}); })} {}

}  // namespace workout::client
```

Create `examples/tui/workout/app/workout_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>

#include "workout_controller.hpp"

/// @file
/// @brief The workout's view: bindings to a `WorkoutController`, nothing else.

namespace workout::client {

/// @brief Builds the workout's view tree.
/// @param controller The controller every binding reads and every control writes. Borrowed: it must outlive the
///        mounted view.
/// @param ctx The frontend's context; the confirmation's Quit button quits it. Borrowed.
/// @return The root node.
[[nodiscard]] morph::ui::Node workoutView(WorkoutController& controller, morph::ui::AppContext& ctx);

}  // namespace workout::client
```

Create `examples/tui/workout/app/workout_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "workout_view.hpp"

#include <cstddef>
#include <functional>
#include <morph/reactive/signal.hpp>
#include <string>
#include <utility>

namespace workout::client {

namespace {

namespace ui = morph::ui;

ui::Node labelled(std::string label, std::function<std::string()> value) {
    return ui::row({.children = {ui::text({.text = std::move(label), .role = ui::TextRole::Muted}),
                                 ui::text({.text = std::move(value)})},
                    .gap = 2});
}

ui::Node dashboard(WorkoutController& controller) {
    auto rider = ui::textInput({.value = [&controller] { return controller.state().rider.get(); },
                                .onChange = [&controller](std::string name) { controller.send(Rename{.rider = std::move(name)}); },
                                .placeholder = "Rider name"});
    auto buttons = ui::row(
        {.children = {ui::button({.label = [&controller] { return controller.sprintLabel(); },
                                  .onClick = controller.action(Sprint{}),
                                  .common = {.enabled = [&controller] { return controller.canSprint(); }}}),
                      ui::button({.label = "Lap", .onClick = controller.action(MarkLap{})})},
         .gap = 2});
    auto laps = ui::forEach<Lap>(
        controller.state().laps, [](Lap const& lap) { return ui::Key{lap.number}; },
        [](morph::reactive::Signal<Lap> const& lap) {
            return ui::text({.text = [lap = &lap] { return lapLine(lap->get()); }});
        });
    return ui::column({.children = {rider, labelled("Elapsed", [&controller] { return controller.elapsedText(); }),
                                    labelled("This lap", [&controller] { return controller.lapText(); }), buttons,
                                    ui::text({.text = "Laps", .role = ui::TextRole::Heading}), laps},
                       .gap = 1});
}

ui::Node statistics(WorkoutController& controller) {
    return ui::column({.children = {labelled("Laps ridden", [&controller] { return controller.lapCountText(); }),
                                    labelled("Best lap", [&controller] { return controller.bestLapText(); }),
                                    labelled("Average lap", [&controller] { return controller.averageLapText(); }),
                                    labelled("Sprints", [&controller] { return controller.sprintCountText(); }),
                                    labelled("Total", [&controller] { return controller.elapsedText(); })},
                       .gap = 1});
}

}  // namespace

ui::Node workoutView(WorkoutController& controller, ui::AppContext& ctx) {
    auto title = ui::row({.children = {ui::text({.text = "Workout", .role = ui::TextRole::Heading}),
                                       ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}}),
                                       ui::text({.text = [&controller] { return controller.statusText(); },
                                                 .role = ui::TextRole::Muted})}});
    // One table: each item's label and what it does, side by side.
    auto actions = ui::menu({.items = {{.label = "Start", .onSelect = controller.action(Start{})},
                                       {.label = "Pause", .onSelect = controller.action(Pause{})},
                                       {.label = "Lap", .onSelect = controller.action(MarkLap{})},
                                       {.label = "Reset", .onSelect = controller.action(Reset{})},
                                       {.label = "Quit", .onSelect = controller.action(AskQuit{})}}});
    auto pages = ui::tabs({.tabs = {{.label = "Dashboard", .node = dashboard(controller)},
                                    {.label = "Statistics", .node = statistics(controller)}},
                           .selected = [&controller] { return controller.state().tab.get(); },
                           .onSelect = [&controller](std::size_t tab) { controller.send(ShowTab{.tab = tab}); }});
    auto confirm = ui::dialog(
        {.open = [&controller] { return controller.state().confirmingQuit.get(); },
         .title = "Quit the workout?",
         .child = ui::row({.children = {ui::button({.label = "Quit", .onClick = [&ctx] { ctx.quit(0); }}),
                                        ui::button({.label = "Keep riding", .onClick = controller.action(CancelQuit{})})},
                           .gap = 2}),
         .onDismiss = controller.action(CancelQuit{})});
    return ui::column({.children = {title, ui::row({.children = {actions, pages}, .gap = 2}), confirm}, .gap = 1});
}

}  // namespace workout::client
```

Create `examples/tui/workout/app/workout_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/app_environment.hpp>
#include <memory>
#include <morph/ui/frontend.hpp>

/// @file
/// @brief The workout dashboard application.

namespace workout::client {

/// @brief Builds the workout application: its controller, ticking on the frontend's scheduler, and its view.
/// @param ctx The frontend's context.
/// @param env The deployment choices; the workout reads none of them.
/// @return The application.
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env);

}  // namespace workout::client
```

Create `examples/tui/workout/app/workout_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "workout_application.hpp"

#include <memory>
#include <morph/ui/view.hpp>

#include "workout_controller.hpp"
#include "workout_view.hpp"

namespace workout::client {

namespace {

class WorkoutApplication final : public morph::ui::Application {
public:
    explicit WorkoutApplication(morph::ui::AppContext& ctx)
        : _ctx{&ctx}, _controller{ctx.runtime(), ctx.scheduler()} {}

    [[nodiscard]] morph::ui::Node view() override { return workoutView(_controller, *_ctx); }

private:
    morph::ui::AppContext* _ctx;
    WorkoutController _controller;
};

}  // namespace

std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                        morph::examples::AppEnvironment const& /*env*/) {
    return std::make_unique<WorkoutApplication>(ctx);
}

}  // namespace workout::client
```

Create `examples/tui/workout/ui/main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <vector>

#include "workout_application.hpp"

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
        return frontend->run(
            [&env](morph::ui::AppContext& ctx) { return workout::client::makeApplication(ctx, env); });
    } catch (std::exception const& error) {
        std::cerr << "workout: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "workout: unknown error\n";
    }
    return 1;
}
```

In `examples/tui/README.md`, add the row:

```markdown
| `workout` | A dashboard driven by a `Store` of signal fields and messages: a one-second ticker on the frontend's scheduler, a menu, Dashboard and Statistics tabs, a keyed lap list, a Sprint button whose `enabled` is bound, a rider-name input and a quit confirmation. The shape every migrated example's controller and view follow. |
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/all --target workout workout_tests && ./build/all/examples/tui/workout/workout_tests`
Expected: PASS, 15 test cases; `build/ex-tui` and `build/ex-qt` build and pass with the other frontend's smoke
case skipped. By hand once: `./build/all/examples/tui/workout/workout --ui=tui` — Start from the menu, the clock
runs, Sprint enables after a second, Quit asks first.
Mutation check: in `_canSprint`, drop `&& state.cooldown.get() == 0`. Expected FAIL in "Sprint needs a ride under
way, a lap started and no cooldown" and in the view test "Start from the menu, a tick and Sprint drive the
dashboard" (`enabled` stays `"true"`). Restore. Second: give `_ticker` a discarded handle
(`static_cast<void>(scheduler.every(…))` in the body, `_ticker` default-constructed). Expected FAIL in "the
ticker counts only while riding". Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/tui
git commit -m "wip(examples-common): examples/tui/workout, a Store-driven dashboard with a ticker

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 12: Whole-part verification and the squash

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict builds and the full suites, in all three configurations**

```bash
for dir in build/all build/ex-tui build/ex-qt; do
    cmake -S . -B "$dir" | grep 'morph: warnings'
    cmake --build "$dir" && ctest --test-dir "$dir" --output-on-failure || break
done
ctest --test-dir build/all -L 'examples-common|examples-tui' --output-on-failure
ctest --test-dir build/all -L ladder -LE stress --output-on-failure
```

Expected: each configure prints `morph: warnings: ... strict=ON`; every build and suite passes. The labelled run
lists `examples_common_app_tests`, `gallery_tests` and `workout_tests` cases (check the count is non-zero:
`ctest --test-dir build/all -N -L 'examples-common|examples-tui' | tail -1`); the ladder run shows every rung
unchanged — this part adds targets and changes none.

- [ ] **Step 2: Sanitizers** (Linux; on macOS use an ASan configure of the same tree)

```bash
cmake --preset clang-asan -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_NET=ON && cmake --build --preset clang-asan \
      --target examples_common_app_tests gallery_tests workout_tests morph_net_tests
for bin in examples/common/app/tests/examples_common_app_tests examples/tui/gallery/gallery_tests \
           examples/tui/workout/workout_tests tests/net/morph_net_tests; do
    bash scripts/check_sanitizer_instrumentation.sh --binary "build/clang-asan/$bin" asan
    "./build/clang-asan/$bin"
done
cmake --preset clang-tsan -DMORPH_BUILD_NET=ON && cmake --build --preset clang-tsan --target morph_net_tests examples_common_app_tests
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/tests/net/morph_net_tests tsan
./build/clang-tsan/tests/net/morph_net_tests "[notifications]"
./build/clang-tsan/examples/common/app/tests/examples_common_app_tests "[transport]"
```

Expected: clean. "a connect notification after the connection is gone touches nothing" and the destroyed-poller
case rely on ASan as their observer; the handler hand-off from the I/O loop to the owner (Task 1, Task 5) is what
TSan checks.

- [ ] **Step 3: clang-tidy over the changed lines** — the recipe in CONTRIBUTING, "Running the `clang-tidy-diff`
  gate locally", configured with `-DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT_QUICK=ON` added to its flags so
  `examples/common/app`, `examples/tui` and both smoke halves have compile commands; diff base
  `origin/master...HEAD`, file count asserted non-zero.

Expected: no findings.

- [ ] **Step 4: Docs and install** — `include/morph/net/socket_backend.hpp` changed.

```bash
cmake -S . -B build/doc -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/doc --target doc
bash scripts/check_install_export.sh
```

Expected: both pass (the install check configures `MORPH_BUILD_NET=ON` and compiles every installed header).

- [ ] **Step 5: WebAssembly** — where an Emscripten toolchain and a wasm Qt kit are available, configure as
  `.github/workflows/wasm-ladder.yml` does (`-DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_CLIENT_ONLY=ON
  -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF …`) and build `morph_ladder_app_common`: it compiles the
  `EM_JS` query reader and the Qt transport there. Where none is available, say so in the hand-off: the
  `wasm-ladder` CI job builds every target of that configure, this one included.

- [ ] **Step 6: Commit any fixes**

```bash
git add -A cmake examples/common examples/tui include/morph/net tests/net docs CHANGELOG.md CMakeLists.txt
git commit -m "wip(examples-common): fixes from the sanitizer, tidy, docs and install gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

- [ ] **Step 7: Squash this part into its one commit**

Follow the master plan's "Squashing a part" procedure with key `examples-common` and this message:

```text
examples/common: Qt-free app environment, transport, poller and test waits; gallery and workout

morph_ladder_app_common gives every example application a toolkit-free
foundation: AppEnvironment::fromArgs, connect() building the Bridge in-process
or over QtWebSocketBackend (Qt Quick) or morph::net's SocketBackend (the TUI)
with a tracked ready() signal, newUuid, id helpers, a Poller over a
refreshed Query, and the Wiring and mapCompletion every example controller
is built from. The testkit gains pumpUntil, FakeAppContext and a frontend
smoke harness; morph_add_rung learns app/, ui/ and tests/smoke/; examples/tui
holds the gallery and workout. SocketBackend now reports connects and drops.
examples/tui is added after the Tests block, not the Demo block: its tests
need include(Catch) and the testkit target.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure's output lists, after the commits of Parts 0–5, this commit as
`examples/common: Qt-free app environment, transport, poller and test waits; gallery and workout`.
