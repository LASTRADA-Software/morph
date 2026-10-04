# Declarative UI, Part 3 — `IoLoopDriver::Caller` and `morph::tui` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Let an `exec::IoLoop` be turned by the thread that built it, then ship `morph::tui` — a compiled,
optional terminal frontend that renders a `morph::ui` view tree through core-cpp's `core::tui` and runs a whole
application (UI, sockets, timers, bridge callbacks) on that one caller-driven loop.

**Architecture:** `IoLoopDriver::Caller` starts no thread and treats the constructing thread as the loop's, so
`runAndWait` and a component's close run inline there, the way the single-threaded WebAssembly build already
works. `morph::tui` is a STATIC library (`morph_tui`): `LoopExecutor` adapts the loop to `exec::IExecutor`;
`Backend` implements `ui::IViewBackend` with one private widget class per widget interface, each owning a
`core::tui::Component` view that the containers arrange in `render()` with a cell-based stack/grid solver;
`Frontend` owns the loop, the executor, the reactive runtime, the `Screen` and a `TuiRuntime`, pumps one dispatch
per input event, coalesces redraws to one per loop turn, and races the pump against a quit signal.

**Tech Stack:** C++23; core-cpp 0.7 (`core::net::EventLoop`, `core::async::{Task, whenAny, AsyncQueue}`,
`core::tui::{Screen, Component, InputField, List, Canvas}`, `core::tui::runtime::TuiRuntime`); libunicode 0.9.3;
Part 1's `morph::reactive`; Part 2's `morph::ui`; Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` §2 (`morph::tui` packaging), §5b, §6,
§7 (tui), §9; `docs/superpowers/specs/2026-10-04-core-cpp-mouse-design.md` (mouse tracking, pointer capture,
`Screen::componentAt`, all provided by core-cpp 0.7 after Part 0).

This is **Part 3 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` commit convention. This part has **two groups**: Tasks 1–4 use key `ioloop` and end by squashing into
`core: IoLoopDriver::Caller, the I/O loop on the calling thread`; Tasks 5–20 use key `tui` and end by squashing into
`tui: morph::tui terminal frontend`. Parts 0, 1 and 2 have landed: core-cpp is pinned at 0.7 (`MouseTracking`,
`Terminal::setMouseTracking`, `Screen::releasePointer`, public `Screen::componentAt`, implicit pointer capture),
and `morph::reactive` and `morph::ui` exist exactly as the interface contract lists them.

## Global Constraints

- Part 1's constraints apply unchanged: SPDX line first, `#pragma once`, naming per `.clang-tidy` (types
  `CamelCase`, functions/variables `camelBack`, private members `_camelBack`, constants `kName`, identifiers ≥ 3
  characters except `i j k x y n fn cb op`), present-tense comments with no history or issue numbers, full Doxygen
  on every symbol of `include/morph/**`, `-Weverything -Werror`, clang-tidy clean, the sign-off trailer.
- `morph::tui` is the only compiled library in `morph` besides Qt's MOC'd `morph_qt_impl`; its public headers
  (`include/morph/tui/`) name `core::tui` types but no private `src/tui/` type. Widgets, the solver, the scheduler
  and the session are private `.hpp`/`.cpp` pairs under `src/tui/`, included as `"tui/<name>.hpp"`.
- New `.cpp` files are analysed by clang-tidy as main files: no `operator[]` on containers
  (`cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` — use `.at()`, iterators or ranges), no
  `switch` over an enum (`-Wswitch-enum` plus `-Wswitch-default` — use `if` chains), no two adjacent parameters
  of one type unless they are used together (`bugprone-easily-swappable-parameters` — coordinates travel as
  `core::tui::Point`), `auto const` for every local that is not modified.
- Every widget's view is a `core::tui::Component` that the widget owns; the widget registers it in the backend's
  `detail::Context` and clears the `Screen`'s focus from it before it is destroyed. `core::tui` holds raw pointers
  to components (children, focus, overlays); nothing in `src/tui/` may leave one dangling.
- Exactly one `Screen::dispatchEvent` per input event, everywhere in `src/tui/` (spec 1 §1, "one dispatch per
  input event").
- Coordinates: `MouseEvent::x/y` are 1-based and relative to the receiving component's `screenBounds()`;
  `Screen::componentAt(row, col)` and `core::tui::Point` are 0-based viewport cells.

## Review Focus

1. **A binding that echoes the text the user just typed back into a TextInput** (the controlled-input loop:
   `onChange` writes a signal, the signal's binding calls `setText` with the same string) must neither move the
   cursor nor fire `onChange` again (Task 10 test "setText with the field's own text keeps the cursor and fires
   nothing").
2. **One input event dispatched more than once** — `core::tui::runtime::runModal` dispatches twice — would type
   every character twice (Task 17 test "each input event is dispatched once: typing h, i reports \"hi\"").
3. **A widget destroyed while it holds keyboard focus** (a remounted ForEach row, a closed Switch case) must not
   leave the `Screen`'s focus pointing at a freed component (Task 8 test "destroying the focused widget clears the
   screen's focus").
4. **Tab inside an open Dialog** must cycle inside it and never reach the tree behind it, because `Screen`'s own
   focus walk does not know overlays exist (Task 12 test "Tab stays inside the open dialog", Task 17 test "Tab
   cycles inside an open dialog and never reaches the tree behind it").
5. **A drag source destroyed mid-gesture** (the card was moved by another client and remounted) must end the drag,
   hide the drag label and highlight, and leave no dangling source pointer (Task 15 test "destroying the drag
   source mid-drag ends the gesture").

---

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/core/io_loop.hpp` | `IoLoopDriver`, `IoLoop(IoLoopDriver)`, `driver()`, Caller-mode `runningHere()` and destructor |
| `tests/test_io_loop.cpp` | Both drivers for the five existing cases; Caller-only cases |
| `tests/test_timeout_scheduler.cpp` | A `TimeoutScheduler` on a Caller loop |
| `tests/net/test_socket_backend.cpp` | An asynchronous round trip with the client on a Caller loop; the synchronous-verb throw |
| `include/morph/tui/loop_executor.hpp`, `src/tui/loop_executor.cpp` | `tui::LoopExecutor` |
| `include/morph/tui/backend.hpp`, `src/tui/backend.cpp` | `tui::Backend` — the factories, `fit`, focus traversal, animation |
| `include/morph/tui/frontend.hpp`, `src/tui/frontend.cpp` | `tui::FrontendConfig`, `tui::Frontend`, `tui::frontendOption` |
| `src/tui/layout.hpp/.cpp` | `tui::layout` — stack and grid solver in cells |
| `src/tui/context.hpp/.cpp` | `detail::Context` — what every widget of one backend shares |
| `src/tui/widget.hpp/.cpp` | `WidgetBase`, `ContainerBase`, `Hosted<>`, `View`, `TuiWidget<>`, `TuiContainer<>`, stack arrangement, focus helpers |
| `src/tui/leaf_widgets.hpp/.cpp` | Text, Button, Checkbox, Spacer, Busy, Slider |
| `src/tui/container_widgets.hpp/.cpp` | Stack, Slot, Grid, Panel, Scroll, Dialog |
| `src/tui/field_widgets.hpp/.cpp` | `FieldView`; TextInput, DateTimeInput, FilePicker; local date-time text |
| `src/tui/list_widgets.hpp/.cpp` | `ListView`; Select (radio, dropdown), Menu, Tabs |
| `src/tui/table_widget.hpp/.cpp` | Table |
| `src/tui/drag.hpp/.cpp` | `DragController` — drag label, drop highlight, target search |
| `src/tui/scheduler.hpp/.cpp` | `LoopScheduler` — `reactive::Scheduler` on `EventLoop` timers |
| `src/tui/session.hpp/.cpp` | `Session` — the `ui::AppContext`, the event pump, draw coalescing, quit |
| `tests/tui/CMakeLists.txt`, `tests/tui/tui_harness.hpp` | The `morph_tui_tests` target; a headless screen + backend harness |
| `tests/tui/test_tui_*.cpp` | One file per task below |
| `CMakeLists.txt`, `cmake/morphConfig.cmake.in`, `CMakePresets.json`, `.github/workflows/ci.yml` | Option, dependency, target, install component, presets, CI |
| `docs/spec/core/executor.md`, `docs/spec/concurrency_and_lifetimes.md` | The I/O loop's drivers |
| `docs/spec/tui/frontend.md`, `docs/spec/README.md`, `docs/ARCHITECTURE.md`, `docs/GETTING-STARTED.md`, `README.md`, `CONTRIBUTING.md`, `CHANGELOG.md` | Spec, maps, pointer, changelog |

## Build and test commands

```bash
# ioloop group (Tasks 1-4): the base tree, plus morph::net for the socket cases
cmake -S . -B build/reactive -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_NET=ON
cmake --build build/reactive --target morph_tests morph_net_tests
./build/reactive/tests/morph_tests "[io_loop],[timeout_scheduler]"
./build/reactive/tests/net/morph_net_tests "[caller]"

# tui group (Tasks 5-19)
cmake -S . -B build/tui -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_TUI=ON
cmake --build build/tui --target morph_tui_tests
./build/tui/tests/tui/morph_tui_tests "[tui]"
```

Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING, "Warnings are
errors"). The first `MORPH_BUILD_TUI=ON` configure without an installed libunicode fetches it, and libunicode's own
configure downloads `UCD.zip` from www.unicode.org.

---

### Task 1: `IoLoopDriver::Caller`

**Files:**
- Modify: `include/morph/core/io_loop.hpp` — the whole file (below)
- Test: `tests/test_io_loop.cpp` — the whole file (below)

**Interfaces:**
- Consumes: `core::net::PlatformLoop`, `EventLoop::runOnce(std::optional<SteadyDuration>)`,
  `EventLoop::runUntilIdle()`, `EventLoop::pendingTimerCount()` (core-cpp `src/core/net/EventLoop.hpp`);
  `morph::exec::runningOn(core::async::IExecutor const&)` (`include/morph/core/executor.hpp:147`);
  `morph::async::detail::TimeoutScheduler` (`include/morph/core/timeout_scheduler.hpp:56`).
- Produces: `morph::exec::IoLoopDriver { OwnThread, Caller }`; `explicit IoLoop(IoLoopDriver = OwnThread)`;
  `[[nodiscard]] IoLoopDriver IoLoop::driver() const noexcept`.

- [ ] **Step 1: Write the failing test**

Replace `tests/test_io_loop.cpp` with:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// morph::exec::IoLoop, the loop every socket, timer and probe of an
// application shares: docs/spec/core/executor.md, "The I/O loop". A case that
// holds for both drivers runs once per driver: on an OwnThread loop the test
// waits for the loop's thread, on a Caller loop the test thread turns the loop.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <stdexcept>
#include <thread>

#include "test_support.hpp"

namespace {

using morph::exec::IoLoop;
using morph::exec::IoLoopDriver;
using namespace std::chrono_literals;

bool onLoop(IoLoop& loop) {
    return morph::exec::runningOn(static_cast<core::async::IExecutor const&>(loop.loop()));
}

/// Waits until @p done holds: by polling while an OwnThread loop's thread runs,
/// or by turning a Caller loop on this thread.
template <class Pred>
bool driveUntil(IoLoop& loop, Pred done, std::chrono::milliseconds budget = 2000ms) {
    if (loop.driver() == IoLoopDriver::OwnThread) {
        return morph::testing::waitUntil(done, morph::testing::WaitBudget{budget});
    }
    auto const deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        static_cast<void>(loop.loop().runOnce(5ms));
    }
    return true;
}

}  // namespace

TEST_CASE("IoLoop: a posted task runs as a task of the loop, on the loop's thread", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    IoLoop loop{driver};
    CHECK(loop.driver() == driver);
    REQUIRE_FALSE(onLoop(loop));
    // Outside a turn, only a Caller loop's driving thread is the loop's.
    CHECK(loop.runningHere() == (driver == IoLoopDriver::Caller));

    std::atomic<bool> ran{false};
    std::atomic<bool> runningHere{false};
    std::atomic<bool> scoped{false};
    std::thread::id where;
    loop.post([&] {
        runningHere = loop.runningHere();
        scoped = onLoop(loop);
        where = std::this_thread::get_id();
        ran = true;
    });
    REQUIRE(driveUntil(loop, [&] { return ran.load(); }));
    CHECK(runningHere.load());
    CHECK(scoped.load());
    CHECK((where == std::this_thread::get_id()) == (driver == IoLoopDriver::Caller));
}

TEST_CASE("IoLoop: runAndWait returns once the task has run, and nests inline", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    IoLoop loop{driver};
    bool ranHere = false;
    bool ranInTurn = false;
    loop.runAndWait([&] {
        ranHere = loop.runningHere();
        ranInTurn = onLoop(loop);
    });
    CHECK(ranHere);
    // A Caller loop runs it inline on the driving thread, outside any turn.
    CHECK(ranInTurn == (driver == IoLoopDriver::OwnThread));

    // From inside a task of the loop, waiting on the loop would wait forever:
    // it runs the nested task inline instead.
    bool nestedRan = false;
    loop.runAndWait([&] { loop.runAndWait([&] { nestedRan = loop.runningHere(); }); });
    CHECK(nestedRan);
}

TEST_CASE("IoLoop: a task that throws is logged and later tasks still run", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    IoLoop loop{driver};
    loop.post([] { throw std::runtime_error{"boom"}; });
    std::atomic<bool> later{false};
    loop.post([&] { later = true; });
    CHECK(driveUntil(loop, [&] { return later.load(); }));
}

TEST_CASE("IoLoop: a weak handle stops posting once the loop is gone", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    auto loop = std::make_unique<IoLoop>(driver);
    auto const weak = loop->weak();
    std::atomic<bool> ran{false};
    REQUIRE(weak.post([&] { ran = true; }));
    REQUIRE(driveUntil(*loop, [&] { return ran.load(); }));
    loop.reset();
    CHECK_FALSE(weak.post([] {}));
}

// OwnThread only: a Caller loop is destroyed on its driving thread outside a
// turn (the next cases), never from one of its own tasks.
TEST_CASE("IoLoop: destroyed from one of its own tasks, it lets the thread finish instead of joining it",
          "[io_loop]") {
    auto loop = std::make_unique<IoLoop>();
    auto const weak = loop->weak();
    std::atomic<bool> destroyed{false};
    loop->post([&] {
        loop.reset();
        destroyed = true;
    });
    REQUIRE(morph::testing::waitUntil([&] { return destroyed.load(); }));
    // The thread held the loop's last share and released it on the way out.
    CHECK(morph::testing::waitUntil([&] { return !weak.post([] {}); }));
}

TEST_CASE("IoLoop (Caller): nothing runs until the driving thread turns the loop", "[io_loop][caller]") {
    IoLoop loop{IoLoopDriver::Caller};
    std::atomic<bool> ran{false};
    loop.post([&] { ran = true; });
    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(ran.load());
    static_cast<void>(loop.loop().runUntilIdle());
    CHECK(ran.load());
}

TEST_CASE("IoLoop (Caller): runAndWait on the driving thread outside a turn runs inline", "[io_loop][caller]") {
    IoLoop loop{IoLoopDriver::Caller};
    REQUIRE(loop.runningHere());
    bool ran = false;
    std::thread::id where;
    loop.runAndWait([&] {
        ran = true;
        where = std::this_thread::get_id();
    });
    CHECK(ran);
    CHECK(where == std::this_thread::get_id());
}

TEST_CASE("IoLoop (Caller): another thread's post and runAndWait run on the driving thread's next turn",
          "[io_loop][caller]") {
    IoLoop loop{IoLoopDriver::Caller};
    std::atomic<bool> posted{false};
    std::atomic<bool> scoped{false};
    std::atomic<bool> waited{false};
    std::atomic<bool> otherRunningHere{true};
    std::thread::id postedOn;
    std::thread::id waitedOn;
    std::thread other{[&] {
        // Recorded, not asserted: Catch2 assertions are not thread-safe.
        otherRunningHere = loop.runningHere();
        loop.post([&] {
            postedOn = std::this_thread::get_id();
            scoped = onLoop(loop);
            posted = true;
        });
        loop.runAndWait([&] { waitedOn = std::this_thread::get_id(); });
        waited = true;
    }};
    bool const done = driveUntil(loop, [&] { return waited.load(); });
    other.join();
    REQUIRE(done);
    CHECK_FALSE(otherRunningHere.load());
    CHECK(posted.load());
    CHECK(scoped.load());
    CHECK(postedOn == std::this_thread::get_id());
    CHECK(waitedOn == std::this_thread::get_id());
}

TEST_CASE("IoLoop (Caller): destroyed outside a turn, it drops queued work unrun", "[io_loop][caller]") {
    auto loop = std::make_unique<IoLoop>(IoLoopDriver::Caller);
    auto const weak = loop->weak();
    auto sentinel = std::make_shared<int>(0);
    bool ran = false;
    loop->post([&ran, held = sentinel] {
        static_cast<void>(held);
        ran = true;
    });
    CHECK(sentinel.use_count() == 2);
    loop.reset();
    CHECK_FALSE(ran);
    CHECK(sentinel.use_count() == 1);
    CHECK_FALSE(weak.post([] {}));
}

TEST_CASE("IoLoop (Caller): a component's close runs inline on the driving thread", "[io_loop][caller]") {
    IoLoop loop{IoLoopDriver::Caller};
    std::atomic<bool> fired{false};
    {
        morph::async::detail::TimeoutScheduler scheduler{loop};
        static_cast<void>(scheduler.schedule(1ms, [&] { fired = true; }));
        // No turn has run, so the arming is still queued. The destructor's
        // close runs inline here; waiting for a turn would never return.
    }
    std::this_thread::sleep_for(5ms);
    static_cast<void>(loop.loop().runUntilIdle());
    CHECK_FALSE(fired.load());
    CHECK(loop.loop().pendingTimerCount() == 0U);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL to compile — `no member named 'IoLoopDriver' in namespace 'morph::exec'`.

- [ ] **Step 3: Implement**

Replace `include/morph/core/io_loop.hpp` with:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/net/EventLoop.hpp>
#include <core/net/PlatformLoop.hpp>
#include <cassert>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include "executor.hpp"
#include "logger.hpp"
#include "profiler.hpp"

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define MORPH_IO_LOOP_HOST_DRIVEN 1
#else
#include <thread>
#endif

/// @file
/// `IoLoop` — the one I/O loop an application runs, and the owner of every
/// socket, timer and probe built on it.
///
/// `morph::net::SocketBackend`, `morph::net::SocketServer`,
/// `TimeoutScheduler` and `offline::NetworkMonitor` own no thread. Each keeps
/// its state on a `core::net::PlatformLoop` and touches it only in tasks that
/// loop runs; its public verbs post to the loop and return. An application
/// constructs one `IoLoop` and hands it to every one of them, so one thread
/// carries every connection, every deadline and the connectivity probe.
///
/// @par Who turns the loop
/// - **`IoLoopDriver::OwnThread`** (native default): the `IoLoop` owns one
///   thread that runs the loop until the `IoLoop` is destroyed.
/// - **`IoLoopDriver::Caller`**: no thread is started. The constructing thread
///   is the loop's and turns it — `loop().runOnce()`, `loop().runUntilIdle()`
///   or a `blockOn()` — so a terminal UI and its sockets, timers and bridge
///   callbacks share that one thread.
/// - **Single-threaded WebAssembly** (`__EMSCRIPTEN__` without
///   `__EMSCRIPTEN_PTHREADS__`): there is no thread to start whichever driver is
///   asked for. The loop is host-driven — the browser's timer pumps it — and
///   the one thread there is is always the loop's, so a verb runs its body
///   inline.

namespace morph::exec {

/// @brief Who turns an `IoLoop`.
enum class IoLoopDriver : std::uint8_t {
    /// The `IoLoop` starts one thread that runs the loop until it is destroyed.
    OwnThread,
    /// No thread is started: the constructing thread turns the loop, and is the
    /// loop's between turns as well as inside them.
    Caller,
};

/// @brief Owns a `core::net::PlatformLoop` and, for `IoLoopDriver::OwnThread`
///        natively, the one thread that runs it.
///
/// Cross-thread surface: `post()`, `runAndWait()`, `runningHere()`, `driver()`
/// and `weak()`. Everything else about the loop — timers, sockets, flows — is
/// touched only in tasks it runs, or, for a `Caller` loop, on its driving
/// thread.
///
/// @par Lifetime
/// Must outlive every component built on it: a component's destructor runs
/// its close on the loop and waits for it, which a stopped loop would never
/// run. Destroy the components first, then the `IoLoop`. A `Caller` loop and
/// its components are destroyed on the driving thread, outside a turn.
class IoLoop {
    /// The loop, shared with the thread that runs it, so that an `IoLoop`
    /// destroyed on its own thread can let that thread finish the turn it is in.
    struct Impl {
        ::core::net::PlatformLoop loop;
    };

public:
    /// @brief A non-owning handle that posts to the loop only while it exists.
    ///
    /// For a callback that another executor runs and that may outlive the
    /// component that armed it — a `RemoteServer` reply arriving on a pool
    /// thread after its transport has gone.
    class Weak {
    public:
        /// @brief Posts @p task if the loop still exists.
        /// @tparam F A copyable callable taking no arguments.
        /// @param task What to run on the loop thread.
        /// @return `false` if the loop is gone and @p task was dropped.
        template <class F>
        bool post(F&& task) const {
            std::shared_ptr<Impl> const impl = _impl.lock();
            if (!impl) {
                return false;
            }
            impl->loop.post(guarded(std::forward<F>(task)));
            return true;
        }

    private:
        friend class IoLoop;
        explicit Weak(std::weak_ptr<Impl> impl) : _impl{std::move(impl)} {}
        std::weak_ptr<Impl> _impl;
    };

#ifndef MORPH_IO_LOOP_HOST_DRIVEN
    /// @brief Creates the loop and, with `IoLoopDriver::OwnThread`, starts the
    ///        thread that runs it.
    /// @param driver Who turns the loop. `Caller` starts no thread and makes the
    ///        constructing thread the loop's.
    explicit IoLoop(IoLoopDriver driver = IoLoopDriver::OwnThread)
        : _impl{std::make_shared<Impl>()}, _driver{driver}, _driverThread{std::this_thread::get_id()} {
        if (driver == IoLoopDriver::OwnThread) {
            _thread = std::thread{[impl = _impl] {
                // One name for every IoLoop thread: an application runs one, and
                // its sockets, deadlines and probe all turn on it.
                MORPH_THREAD_NAME("morph.io");
                impl->loop.run();
            }};
        }
    }

    /// @brief Stops the loop and joins its thread; a `Caller` loop is
    ///        destroyed in place.
    ///
    /// Work still queued is dropped by the loop's own teardown, not run. On the
    /// loop's own thread — a task that dropped the last owner — an `OwnThread`
    /// loop cannot join: it asks the loop to stop and lets the thread end with
    /// the turn it is in, the loop kept alive by the thread's own share of it.
    /// A `Caller` loop has no thread to stop: destroyed outside a turn nothing
    /// is running it, so its teardown is serialised with dispatch. Destroyed
    /// inside one of its own turns, that turn would return into a destroyed
    /// loop; an assertion refuses it.
    ~IoLoop() {
        if (_driver == IoLoopDriver::Caller) {
            assert(!::morph::exec::runningOn(static_cast<::core::async::IExecutor const&>(_impl->loop)) &&
                   "a caller-driven IoLoop must be destroyed outside its own turns");
            return;
        }
        _impl->loop.stop();
        if (_thread.get_id() == std::this_thread::get_id()) {
            _thread.detach();
        } else if (_thread.joinable()) {
            _thread.join();
        }
    }
#else
    /// @brief Creates the loop. Starts no thread whichever driver is asked for:
    ///        the browser's timer pumps it.
    /// @param driver Recorded and reported by `driver()`; both drivers behave
    ///        alike here.
    explicit IoLoop(IoLoopDriver driver = IoLoopDriver::OwnThread) : _impl{std::make_shared<Impl>()}, _driver{driver} {}

    /// @brief Destroys the loop, dropping whatever it still holds.
    ~IoLoop() = default;
#endif

    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;
    IoLoop(IoLoop&&) = delete;
    IoLoop& operator=(IoLoop&&) = delete;

    /// @brief The loop itself, for a component arming timers or opening
    ///        sockets from inside one of its tasks, and for the driving thread
    ///        of a `Caller` loop to turn it.
    /// @return The platform loop; valid for this object's lifetime.
    [[nodiscard]] ::core::net::EventLoop& loop() noexcept { return _impl->loop; }

    /// @brief Who turns this loop.
    /// @return The driver passed at construction.
    [[nodiscard]] IoLoopDriver driver() const noexcept { return _driver; }

    /// @brief Whether the calling thread may touch the loop's state directly.
    ///
    /// Natively that is the loop's own `ExecutorScope`, stated for every turn,
    /// and — for a `Caller` loop — also the driving thread between turns, where
    /// nothing else can be running the loop. Under single-threaded WebAssembly
    /// there is one thread, so the answer is always yes.
    /// @return True inside one of the loop's tasks, or on a `Caller` loop's
    ///         driving thread.
    [[nodiscard]] bool runningHere() const noexcept {
#ifndef MORPH_IO_LOOP_HOST_DRIVEN
        return ::morph::exec::runningOn(static_cast<::core::async::IExecutor const&>(_impl->loop)) ||
               (_driver == IoLoopDriver::Caller && std::this_thread::get_id() == _driverThread);
#else
        return true;
#endif
    }

    /// @brief Queues @p task to run on the loop thread, in a later turn.
    ///
    /// A throw out of @p task is logged and swallowed: one escaping a turn
    /// would end the loop's thread.
    /// @tparam F A copyable callable taking no arguments.
    /// @param task What to run.
    template <class F>
    void post(F&& task) {
        _impl->loop.post(guarded(std::forward<F>(task)));
    }

    /// @brief Runs @p task on the loop and returns once it has run.
    ///
    /// Inline where `runningHere()` holds, so a task of the loop — or a
    /// `Caller` loop's driving thread — never waits on itself. For teardown and
    /// for the few verbs that must answer with the loop's own result. A task
    /// the loop drops unrun (it was destroyed first) ends the wait rather than
    /// hanging it.
    /// @tparam F A copyable callable taking no arguments.
    /// @param task What to run.
    template <class F>
    void runAndWait(F&& task) {
        if (runningHere()) {
            std::forward<F>(task)();
            return;
        }
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> finished = done->get_future();
        post([task = std::forward<F>(task), done]() mutable {
            task();
            done->set_value();
        });
        try {
            finished.get();
        } catch (const std::future_error&) {  // NOLINT(bugprone-empty-catch)
            // Dropped unrun, or it threw (logged by `post`): nothing is left to
            // wait for either way.
        }
    }

    /// @brief A handle that posts only while this loop exists.
    /// @return The weak handle.
    [[nodiscard]] Weak weak() const { return Weak{_impl}; }

private:
    template <class F>
    static auto guarded(F&& task) {
        return [task = std::forward<F>(task)]() mutable noexcept {
            try {
                task();
            } catch (const std::exception& exc) {
                ::morph::log::logError("[io-loop] task threw: {}", exc.what());
            } catch (...) {
                ::morph::log::logError("[io-loop] task threw an unknown exception");
            }
        };
    }

    std::shared_ptr<Impl> _impl;
    IoLoopDriver _driver;
#ifndef MORPH_IO_LOOP_HOST_DRIVEN
    std::thread::id _driverThread;
    std::thread _thread;
#endif
};

}  // namespace morph::exec
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[io_loop]"`
Expected: PASS — 10 test cases (the four two-driver cases count once each, their sections twice).

Mutation check: in `runningHere()`, delete the `|| (_driver == IoLoopDriver::Caller && ...)` clause and rebuild.
Expected: FAIL at `REQUIRE(loop.runningHere())` in "runAndWait on the driving thread outside a turn runs inline"
(the REQUIRE stops the case before `runAndWait` would wait for a turn nobody drives), and FAIL in "a posted task
runs as a task of the loop" on the `Caller` section. Restore the clause.

- [ ] **Step 5: Commit**

```bash
git add include/morph/core/io_loop.hpp tests/test_io_loop.cpp
git commit -m "wip(ioloop): IoLoopDriver::Caller, the I/O loop on the calling thread

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `TimeoutScheduler` and `SocketBackend` on a Caller loop

**Files:**
- Modify: `tests/test_timeout_scheduler.cpp` — append one case at the end
- Modify: `tests/net/test_socket_backend.cpp` — append two cases at the end of the file (after the last
  `TEST_CASE`, "a synchronous verb called on the loop's own thread fails instead of waiting on itself")
- Test: the two files above

**Interfaces:**
- Consumes: `IoLoop(IoLoopDriver::Caller)` (Task 1); `SocketBackend(IoLoop&, std::string_view)`,
  `SocketBackend::bindModel(BindRequest, IExecutor&)`, `SocketBackend::execute(ModelId, ActionCall, IExecutor*)`,
  `SocketBackend::registerModel` (`include/morph/net/socket_backend.hpp:105, 249, 346, 175`); the file-local helpers
  `privateBind()` (`tests/net/test_socket_backend.cpp:1707`) and `echoCall()` (`:2215`), both in that file's
  anonymous namespaces above the append point.
- Produces: nothing new; it proves the components already built on `IoLoop` work on a Caller loop.

- [ ] **Step 1: Write the tests**

Append to `tests/test_timeout_scheduler.cpp`:

```cpp
TEST_CASE("TimeoutScheduler: on a caller-driven loop, a callback fires in the driving thread's turn",
          "[timeout_scheduler][caller]") {
    morph::exec::IoLoop loop{morph::exec::IoLoopDriver::Caller};
    TimeoutScheduler scheduler{loop};
    bool fired = false;
    bool cancelledFired = false;
    std::thread::id where;
    static_cast<void>(scheduler.schedule(5ms, [&] {
        fired = true;
        where = std::this_thread::get_id();
    }));
    auto const cancelled = scheduler.schedule(5ms, [&] { cancelledFired = true; });
    scheduler.cancel(cancelled);

    auto const deadline = std::chrono::steady_clock::now() + 2s;
    while (!fired && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(loop.loop().runOnce(5ms));
    }
    static_cast<void>(loop.loop().runOnce(20ms));
    CHECK(fired);
    CHECK(where == std::this_thread::get_id());
    CHECK_FALSE(cancelledFired);
}
```

Append to `tests/net/test_socket_backend.cpp`:

```cpp
// ── A client on a caller-driven loop ────────────────────────────────────────
//
// The client's IoLoop has no thread: this test thread turns it, the way a TUI
// application's one loop is turned by its input pump. The server keeps a loop
// of its own, so only the client side is caller-driven.

TEST_CASE("SocketBackend: on a caller-driven loop, an asynchronous round trip completes in the driving thread's turns",
          "[net][socket_backend][caller]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    morph::exec::IoLoop loop{morph::exec::IoLoopDriver::Caller};
    morph::exec::MainThreadExecutor cbOwner;
    auto backend = std::make_unique<morph::net::SocketBackend>(
        loop, "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port())));
    auto const drive = [&](auto done) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            static_cast<void>(loop.loop().runOnce(std::chrono::milliseconds{5}));
            static_cast<void>(cbOwner.runOnce());
        }
        return true;
    };

    // On the driving thread waitForConnected cannot wait for the loop: it answers at once.
    REQUIRE(drive([&] { return backend->waitForConnected(std::chrono::milliseconds{0}); }));

    std::optional<morph::exec::detail::ModelId> mid;
    auto bound = backend->bindModel(privateBind("SbEchoModel"), cbOwner);
    bound.then([&](morph::exec::detail::ModelId boundId) { mid = boundId; }).onError([](const std::exception_ptr&) {});
    REQUIRE(drive([&] { return mid.has_value(); }));

    std::optional<std::string> reply;
    std::thread::id repliedOn;
    auto executed = backend->execute(*mid, echoCall(), &cbOwner);
    executed
        .then([&](const std::shared_ptr<void>& result) {
            reply = *std::static_pointer_cast<std::string>(result);
            repliedOn = std::this_thread::get_id();
        })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(drive([&] { return reply.has_value(); }));
    CHECK(*reply == "7");
    CHECK(repliedOn == std::this_thread::get_id());

    // The close runs inline on the driving thread, outside a turn, with the
    // reader flow still parked; the loop's teardown unwinds it afterwards.
    backend.reset();
}

TEST_CASE("SocketBackend: on a caller-driven loop, a synchronous verb on the driving thread throws instead of waiting",
          "[net][socket_backend][caller]") {
    morph::exec::IoLoop loop{morph::exec::IoLoopDriver::Caller};
    // Never driven: the throw comes before any I/O, and the destructor's close
    // runs inline with the connect still queued.
    morph::net::SocketBackend backend{loop, "ws://127.0.0.1:9"};
    CHECK_THROWS_WITH(static_cast<void>(backend.registerModel("SbEchoModel", nullptr)),
                      Catch::Matchers::ContainsSubstring("cannot wait on the I/O loop's own thread"));
}
```

Add `#include <optional>` to `tests/net/test_socket_backend.cpp`'s includes (alphabetically after `<mutex>`).

- [ ] **Step 2: Run them**

Run:

```bash
cmake --build build/reactive --target morph_tests morph_net_tests && ./build/reactive/tests/morph_tests "[caller]" && ./build/reactive/tests/net/morph_net_tests "[caller]"
```
Expected: PASS — 1 case and 2 cases. These test behaviour Task 1 already made true, so there is no red step; the
mutation below is what shows they measure it.

- [ ] **Step 3: Prove the cases can fail**

In `include/morph/core/io_loop.hpp`, change the `Caller` clause of `runningHere()` to `false &&` and rebuild.
Expected: "a synchronous verb on the driving thread throws instead of waiting" FAILS (`registerModel` now posts
and waits for a turn — the case hangs until ctest's 120 s timeout, which is the failure); run it alone with
`timeout 20 ./build/reactive/tests/net/morph_net_tests "*synchronous verb on the driving thread*"` and read the
non-zero exit. Restore the clause.

- [ ] **Step 4: Commit**

```bash
git add tests/test_timeout_scheduler.cpp tests/net/test_socket_backend.cpp
git commit -m "wip(ioloop): TimeoutScheduler and SocketBackend on a caller-driven loop

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: Specify the drivers

**Files:**
- Modify: `docs/spec/core/executor.md` — "Type overview" (the sentence "There are eleven types…" and its table),
  and the whole "The I/O loop — `IoLoop`" section
- Modify: `docs/spec/concurrency_and_lifetimes.md` — the `exec::IoLoop` row of the thread-roles table
- Modify: `CHANGELOG.md` — `## [Unreleased]` → `### Added` (the heading Part 1 created), appended as its last
  entry

**Interfaces:**
- Consumes: the API exactly as Task 1 ships it.
- Produces: the authoritative description the TUI frontend (Task 17) and Part 6's transport rely on.

- [ ] **Step 1: Update `docs/spec/core/executor.md`**

In "Type overview", change "There are eleven types and one free function" to "There are twelve types and one free
function", and add this row after the `IoLoop` row:

```markdown
| `IoLoopDriver` | `morph::exec` (in `io_loop.hpp`) | Who turns an `IoLoop`: `OwnThread` (its own thread) or `Caller` (the constructing thread); see [the I/O loop](#the-io-loop--ioloop). |
```

Replace the section from `## The I/O loop — \`IoLoop\`` up to (not including) `## Failure modes` with:

```markdown
## The I/O loop — `IoLoop`

`IoLoop` owns a `core::net::PlatformLoop`. Who turns it is fixed at
construction by `IoLoopDriver`:

- **`OwnThread`** (the default): the `IoLoop` starts one thread that runs the
  loop; its destructor stops the loop and joins that thread.
- **`Caller`**: no thread is started. The constructing thread is the loop's and
  turns it — `loop().runOnce()`, `loop().runUntilIdle()`, or a `blockOn()` such
  as `core::tui::runtime::TuiRuntime`'s. A terminal application runs this way,
  so its UI, sockets, timers and bridge callbacks share one thread.
- Under single-threaded WebAssembly no thread exists to start, whichever driver
  is asked for; the browser's timer pumps the loop.

An application constructs one and passes it to every component that does I/O
or keeps time — `morph::net::SocketBackend`, `morph::net::SocketServer`,
`TimeoutScheduler`, `offline::NetworkMonitor` — each of which keeps its state
on the loop and touches it only in the loop's tasks.

| Member | Cross-thread? | What it does |
|---|---|---|
| `IoLoop(driver)` | — | Creates the loop; `OwnThread` also starts its thread. |
| `driver()` | yes | The `IoLoopDriver` it was built with. |
| `loop()` | returns a reference | The `core::net::EventLoop`, for timers, sockets and flows armed from inside a task of it, and for a `Caller` loop's driving thread to turn it. |
| `post(task)` | yes | Queues `task` for a later turn. A throw out of it is logged and swallowed: one escaping a turn would end the loop's thread. |
| `runAndWait(task)` | yes | Runs `task` on the loop and returns once it has run — inline where `runningHere()` holds, so a task never waits on itself. A task the loop drops unrun ends the wait. Components use it for teardown and for the verbs that must answer (`SocketServer::listen`). |
| `runningHere()` | yes | Whether the caller may touch the loop's state: inside one of the loop's tasks, or — for a `Caller` loop — anywhere on the driving thread, between turns included. Always true under single-threaded WebAssembly. |
| `weak()` | yes | A handle whose `post` is a no-op returning `false` once the loop is gone — for a callback another executor runs, such as a `RemoteServer` reply. |

**It must outlive every component built on it.** Their destructors run their
close on the loop and wait for it. Destroyed on its own thread (a task dropped
the last owner), an `OwnThread` loop cannot join: it stops the loop and
detaches, and the thread's own share of the loop keeps it alive until the turn
it is in ends.

**A `Caller` loop.** `runningHere()` holds on the driving thread between turns,
because nothing else can be running the loop then: core-cpp's
`EventLoop::teardownIsSerialisedWithDispatch()` holds, so the loop's state may
be touched directly. That is why `runAndWait` and a component's close run
inline there instead of waiting for a turn only the waiting thread could drive.
Other threads still `post()`, `weak().post()` and `runAndWait()`, which waits
for the driving thread's next turn. The loop and its components are destroyed
on the driving thread, outside a turn: a component's close runs inline, and the
loop's own teardown drops whatever is still queued without running it.
Destroying a `Caller` loop inside one of its own turns is refused by an
assertion, because that turn would return into a destroyed loop. A synchronous
`SocketBackend` verb called on the driving thread throws, as it does on an
`OwnThread` loop's thread: it would wait for a reply only that thread can
deliver.
```

- [ ] **Step 2: Update `docs/spec/concurrency_and_lifetimes.md`**

Replace the `exec::IoLoop` row of the thread-roles table with:

```markdown
| `exec::IoLoop` | One thread natively (`IoLoopDriver::OwnThread`), or the constructing thread (`IoLoopDriver::Caller`); host-pumped under single-threaded WebAssembly | The I/O loop (`io_loop.hpp`): a core-cpp `PlatformLoop` that owns every `morph::net` socket, every `TimeoutScheduler` timer and `NetworkMonitor`'s probe. The application constructs one and injects it into each; `IoLoop::post` is the one way in from another thread. A `Caller` loop is turned by the thread that built it — a terminal UI's input pump — and is that thread's between turns too. |
```

- [ ] **Step 3: Add the changelog entry**

Append to `CHANGELOG.md`, `## [Unreleased]` → `### Added`:

```markdown
- **`exec::IoLoopDriver::Caller`: an I/O loop turned by the thread that built it.**
  `IoLoop(IoLoopDriver::Caller)` starts no thread; the constructing thread turns
  the loop (`loop().runOnce()`, `runUntilIdle()`, a `blockOn()`), and
  `runningHere()` holds there between turns too, so `runAndWait` and a
  component's close run inline. Other threads still `post()`. `IoLoop()` keeps
  its own thread (`IoLoopDriver::OwnThread`), unchanged. Specified in
  `docs/spec/core/executor.md`, "The I/O loop".
```

- [ ] **Step 4: Build the docs with warnings as errors**

```bash
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
```

Expected: exits 0. A warning naming `IoLoopDriver` or `IoLoop` is a missing brief or `@param`: fix the header.

- [ ] **Step 5: Commit**

```bash
git add docs/spec/core/executor.md docs/spec/concurrency_and_lifetimes.md CHANGELOG.md
git commit -m "wip(ioloop): specify the I/O loop's drivers

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: Verify and squash the `ioloop` group

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the full suites**

```bash
cmake --build build/reactive && ctest --test-dir build/reactive --output-on-failure
```

Expected: every test passes (`morph_tests` and `morph_net_tests`).

- [ ] **Step 2: Sanitizers** (Linux; on macOS an ASan configure of the same tree)

```bash
cmake --preset clang-tsan -DMORPH_BUILD_NET=ON && cmake --build --preset clang-tsan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/tests/morph_tests tsan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/tests/net/morph_net_tests tsan
./build/clang-tsan/tests/morph_tests "[io_loop],[timeout_scheduler]"
./build/clang-tsan/tests/net/morph_net_tests "[caller]"
cmake --preset clang-asan -DMORPH_BUILD_NET=ON && cmake --build --preset clang-asan
./build/clang-asan/tests/morph_tests "[io_loop],[timeout_scheduler]"
./build/clang-asan/tests/net/morph_net_tests "[caller]"
```

Expected: clean. The cross-thread `post`/`runAndWait` case is what TSan watches; the round trip's `backend.reset()`
with a parked reader followed by the loop's teardown is what ASan watches.

- [ ] **Step 3: clang-tidy over the changed lines** — CONTRIBUTING's "Running the `clang-tidy-diff` gate locally"
  recipe with `origin/master...HEAD` and the file count asserted non-zero. Expected: no findings.

- [ ] **Step 4: Commit any fixes**, as `wip(ioloop): fixes from the sanitizer and tidy gates`; skip when there
  were none, and say so in the hand-off.

- [ ] **Step 5: Squash the group**

Follow the master plan's "Squashing a part" procedure with `key=ioloop` and this message:

```text
core: IoLoopDriver::Caller, the I/O loop on the calling thread

IoLoop(IoLoopDriver::Caller) starts no thread: the constructing thread
turns the loop, and runningHere() holds there between turns as well, so
runAndWait and a component's close run inline instead of waiting for a
turn only that thread could drive. It generalises the single-threaded
WebAssembly build's host-driven loop to native builds, so a terminal
UI's sockets, timers, bridge callbacks and input share one thread. The
IoLoop tests run under both drivers; TimeoutScheduler and an
asynchronous SocketBackend round trip run on a Caller loop.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure must list, after Parts 0–2's commits,
`core: IoLoopDriver::Caller, the I/O loop on the calling thread`.

---
### Task 5: `MORPH_BUILD_TUI`, the `morph_tui` target and `LoopExecutor`

**Files:**
- Modify: `CMakeLists.txt` — five places, each named by the text around it (below)
- Create: `include/morph/tui/loop_executor.hpp`, `src/tui/loop_executor.cpp`
- Create: `tests/tui/CMakeLists.txt`
- Test: `tests/tui/test_tui_loop_executor.cpp`

**Interfaces:**
- Consumes: `core::net::EventLoop::post(std::function<void()>)` (core-cpp `EventLoop.hpp`);
  `core::async::ExecutorScope` (core-cpp `async/ExecutorContext.hpp`); `morph::exec::IExecutor`,
  `IExecutor::coreExecutor()`, `runningOn(IExecutor&)` (`include/morph/core/executor.hpp:66–147`);
  `morph::log::logError`, `ScopedLoggerOverride` (`include/morph/core/logger.hpp:256, 279`); CMake helpers
  `apply_warnings`, `apply_sanitizers`, `apply_coverage` (`cmake/compiler_options.cmake:474, 539, 742`) and
  `morph_use_cpm` (`CMakeLists.txt:163`).
- Produces: option `MORPH_BUILD_TUI`; variable `MORPH_LIBUNICODE_VERSION`; targets `morph_tui` / `morph::tui` and
  `morph_tui_tests`; `morph::tui::LoopExecutor(core::net::EventLoop&)` with `post(std::function<void()>)`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_loop_executor.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/net/PlatformLoop.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/tui/loop_executor.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using morph::tui::LoopExecutor;
using namespace std::chrono_literals;

TEST_CASE("tui::LoopExecutor: a posted task runs in a later loop turn, with the executor current",
          "[tui][loop_executor]") {
    core::net::PlatformLoop loop;
    LoopExecutor executor{loop};
    bool ran = false;
    bool current = false;
    executor.post([&] {
        ran = true;
        current = morph::exec::runningOn(executor);
    });
    CHECK_FALSE(ran);
    static_cast<void>(loop.runUntilIdle());
    CHECK(ran);
    CHECK(current);
}

TEST_CASE("tui::LoopExecutor: a task still queued when the executor is destroyed is dropped", "[tui][loop_executor]") {
    core::net::PlatformLoop loop;
    bool ran = false;
    {
        LoopExecutor executor{loop};
        executor.post([&] { ran = true; });
    }
    static_cast<void>(loop.runUntilIdle());
    CHECK_FALSE(ran);
}

TEST_CASE("tui::LoopExecutor: a throwing task is logged, and later tasks still run", "[tui][loop_executor]") {
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride const capture{
        [&](morph::log::LogLevel, std::string_view message) { logged.emplace_back(message); }};
    core::net::PlatformLoop loop;
    LoopExecutor executor{loop};
    bool later = false;
    executor.post([] { throw std::runtime_error{"boom"}; });
    executor.post([&] { later = true; });
    static_cast<void>(loop.runUntilIdle());
    CHECK(later);
    REQUIRE(logged.size() == 1);
    CHECK(logged.front().find("boom") != std::string::npos);
}

TEST_CASE("tui::LoopExecutor: a post from another thread runs on the thread turning the loop",
          "[tui][loop_executor]") {
    core::net::PlatformLoop loop;
    LoopExecutor executor{loop};
    std::atomic<bool> ran{false};
    std::thread::id where;
    std::thread other{[&] {
        executor.post([&] {
            where = std::this_thread::get_id();
            ran = true;
        });
    }};
    other.join();
    auto const deadline = std::chrono::steady_clock::now() + 2s;
    while (!ran.load() && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(loop.runOnce(5ms));
    }
    REQUIRE(ran.load());
    CHECK(where == std::this_thread::get_id());
}
```

Create `tests/tui/CMakeLists.txt`:

```cmake
add_executable(morph_tui_tests
    test_tui_loop_executor.cpp
)

target_link_libraries(morph_tui_tests
    PRIVATE
        morph::tui
        morph_test_main
)
# The widgets, the layout solver and the session are private to morph_tui
# (src/tui/); their tests include them as "tui/<name>.hpp".
target_include_directories(morph_tui_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

apply_warnings(morph_tui_tests)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_tui_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_tui_tests)
endif()

include(Catch)
# LABELS "tui" is what makes `ctest -L tui` select this suite: Catch2 tags are
# not ctest labels (see tests/net/CMakeLists.txt).
catch_discover_tests(morph_tui_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "tui")
```

- [ ] **Step 2: Wire the option, the dependency and the target**

In `CMakeLists.txt`:

**(a)** After the line `option(MORPH_BUILD_NET ...)`, add:

```cmake
option(MORPH_BUILD_TUI           "Build morph::tui, the terminal frontend over core-cpp's core::tui (a compiled library; fetches libunicode if not installed)" OFF)
# core-cpp builds no core::tui under Emscripten, so there is nothing for
# morph::tui to wrap there.
if(MORPH_BUILD_TUI AND EMSCRIPTEN)
    message(WARNING "MORPH_BUILD_TUI is ignored under Emscripten: core-cpp builds no core::tui there.")
    set(MORPH_BUILD_TUI OFF)
endif()
```

**(b)** Directly before the line `# ── core-cpp ───…`, add:

```cmake
# ── libunicode (MORPH_BUILD_TUI) ────────────────────────────────────────────
# core::tui segments graphemes and measures display widths with libunicode.
# core-cpp is added below with CORE_CPP_FETCH_DEPS OFF, so it takes the
# unicode::unicode target its parent already provides; this block provides it.
# Found first and fetched only when no installed one satisfies the bound, as
# glaze and Tracy are. The version and options are core-cpp's own row for the
# same dependency (its cmake/CoreCppDependencies.cmake), so an application that
# links both libraries gets one libunicode. A fetch runs libunicode's own
# configure, which downloads UCD.zip from www.unicode.org.
set(MORPH_LIBUNICODE_VERSION 0.9.3)
if(MORPH_BUILD_TUI AND NOT TARGET unicode::unicode)
    find_package(libunicode ${MORPH_LIBUNICODE_VERSION} CONFIG QUIET)
    if(NOT libunicode_FOUND)
        morph_use_cpm(libunicode)
        CPMAddPackage(
            NAME libunicode
            VERSION ${MORPH_LIBUNICODE_VERSION}
            GITHUB_REPOSITORY contour-terminal/libunicode
            GIT_TAG v${MORPH_LIBUNICODE_VERSION}
            EXCLUDE_FROM_ALL YES
            SYSTEM YES
            OPTIONS "LIBUNICODE_TESTING OFF" "LIBUNICODE_BENCHMARK OFF" "LIBUNICODE_TOOLS OFF"
                    "LIBUNICODE_EXAMPLES OFF" "PEDANTIC_COMPILER OFF" "PEDANTIC_COMPILER_WERROR OFF"
                    "BUILD_SHARED_LIBS OFF")
    endif()
endif()
```

**(c)** In the core-cpp block: in its leading comment, replace "none of core-cpp's own tests, examples, TUI or TLS"
with "none of core-cpp's own tests, examples or TLS, and its TUI only with MORPH_BUILD_TUI (without image
decoding)"; in the `CPMAddPackage(NAME core-cpp …)` call replace `"CORE_CPP_WITH_TUI OFF"` with
`"CORE_CPP_WITH_TUI ${MORPH_BUILD_TUI}" "CORE_CPP_WITH_IMAGES OFF"`. After that block's closing `endif()` (the one
after `unset(_morph_core_cpp_exclude_from_all)`), add:

```cmake
# A core-cpp found installed brings whatever modules it was built with. One
# built with CORE_CPP_WITH_TUI=OFF has no core::tui, and morph::tui would fail
# much later, at link time, naming a target nobody created.
if(MORPH_BUILD_TUI AND NOT TARGET core::tui)
    message(FATAL_ERROR
        "MORPH_BUILD_TUI=ON, but the core-cpp this configure found (${core-cpp_DIR}) was built without "
        "core::tui (CORE_CPP_WITH_TUI=OFF). Install a core-cpp built with CORE_CPP_WITH_TUI=ON, or take "
        "it off CMAKE_PREFIX_PATH so morph fetches one.")
endif()
```

**(d)** Directly before the line that starts
`# ── Guard: every public header must belong to some target's FILE_SET`, add:

```cmake
# ── morph::tui terminal frontend (optional, compiled) ───────────────────────
# A STATIC library, unlike every other morph component: its widgets are
# core::tui components, compiled once here rather than in every consumer.
# Created before the public-header guard below, so include/morph/tui/ is
# checked whenever the option is on. Its tests are added after the Tests
# section, which provides Catch2.
if(MORPH_BUILD_TUI)
    add_library(morph_tui STATIC
        src/tui/loop_executor.cpp
    )
    add_library(morph::tui ALIAS morph_tui)
    target_sources(morph_tui
        PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/morph/tui/loop_executor.hpp
    )
    # Same standalone-compile check the core target gets.
    set_target_properties(morph_tui PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
    target_include_directories(morph_tui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(morph_tui PUBLIC morph core::tui)
    apply_warnings(morph_tui)
    # Every consumer of this archive (tests/tui, the examples that choose a
    # frontend) carries an AF_SANITIZER block of its own, which is what makes
    # instrumenting a static library safe (see morph_qt_impl below).
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(morph_tui ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(morph_tui)
    endif()
endif()
```

In the guard below it, add `"include/morph/tui/:morph_tui"` as the last entry of `set(_morph_optional_components …)`
and `morph_tui` to `foreach(_morph_target IN ITEMS morph morph_net morph_qt morph_qt_forms morph_offline_sqlite)`
(it becomes `… morph_offline_sqlite morph_tui)`). Also extend that block's comment list "(morph::net, morph::qt,
morph::qt_forms, morph::offline_sqlite)" with ", morph::tui".

**(e)** After the `# ── morph::net raw-socket WebSocket transport …` block's final `endif()`, add:

```cmake
# ── morph::tui tests (optional) ─────────────────────────────────────────────
# The target itself is created before the public-header guard; its suite is
# added here, after the Tests section has provided Catch2 and morph_test_main.
if(MORPH_BUILD_TUI AND MORPH_BUILD_TESTS)
    add_subdirectory(tests/tui)
endif()
```

- [ ] **Step 3: Run it to verify it fails**

Run: `cmake -S . -B build/tui -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_TUI=ON`
Expected: configure FAILS — `Cannot find source file: src/tui/loop_executor.cpp`.

- [ ] **Step 4: Implement**

Create `include/morph/tui/loop_executor.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/net/EventLoop.hpp>
#include <functional>
#include <memory>

#include "../attributes.hpp"
#include "../core/executor.hpp"

/// @file
/// @brief `morph::tui::LoopExecutor`: a morph executor whose tasks run in turns of a core-cpp event loop.
///
/// Specified in `docs/spec/tui/frontend.md`, "The loop executor".

namespace morph::tui {

/// @brief Runs every posted task in a later turn of one `core::net::EventLoop`, with this executor current.
///
/// The TUI frontend's reactive `Runtime` is owned by one, and every `BridgeHandler` an application builds receives
/// it as its callback executor, so model replies, timers, sockets and input all run on the thread that turns the
/// loop. `post()` may be called from any thread. A task still queued when the executor is destroyed is dropped,
/// and a task that throws is logged and does not stop the loop.
class LoopExecutor final : public exec::IExecutor {
public:
    /// @param loop The loop to post to. Borrowed: it must outlive this executor.
    explicit LoopExecutor(::core::net::EventLoop& loop MORPH_LIFETIMEBOUND);

    /// @brief Destroys the executor; tasks it posted that have not run yet are dropped when the loop reaches them.
    ~LoopExecutor() override;

    LoopExecutor(LoopExecutor const&) = delete;
    LoopExecutor& operator=(LoopExecutor const&) = delete;
    LoopExecutor(LoopExecutor&&) = delete;
    LoopExecutor& operator=(LoopExecutor&&) = delete;

    /// @brief Queues @p task for a later turn of the loop.
    /// @param task What to run; inside it `exec::runningOn(*this)` holds.
    void post(std::function<void()> task) override;

private:
    ::core::net::EventLoop* _loop;
    /// Read by each queued task: once this executor is gone the task is dropped. A box rather than the executor,
    /// because a task outlives the executor that posted it.
    std::shared_ptr<LoopExecutor*> _alive;
};

}  // namespace morph::tui
```

Create `src/tui/loop_executor.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <core/async/ExecutorContext.hpp>
#include <exception>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/tui/loop_executor.hpp>
#include <utility>

namespace morph::tui {

LoopExecutor::LoopExecutor(::core::net::EventLoop& loop) : _loop{&loop}, _alive{std::make_shared<LoopExecutor*>(this)} {}

LoopExecutor::~LoopExecutor() = default;

void LoopExecutor::post(std::function<void()> task) {
    _loop->post([alive = std::weak_ptr<LoopExecutor*>{_alive}, task = std::move(task)] {
        auto const self = alive.lock();
        if (!self) {
            return;
        }
        ::core::async::ExecutorScope const scope{(*self)->coreExecutor()};
        // Caught here: one exception escaping a loop turn would end the frontend's event loop.
        try {
            task();
        } catch (std::exception const& failure) {
            ::morph::log::logError("[tui] loop task threw: {}", failure.what());
        } catch (...) {
            ::morph::log::logError("[tui] loop task threw an unknown exception");
        }
    });
}

}  // namespace morph::tui
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake -S . -B build/tui -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_TUI=ON
cmake --build build/tui --target morph_tui_tests morph_verify_interface_header_sets
./build/tui/tests/tui/morph_tui_tests "[loop_executor]"
```

Expected: configure prints `morph: warnings: ... strict=ON` and `-- morph: warnings: '-Weverything' verified on N
target(s)` with N one higher than `build/reactive`'s; the build passes (the header set check compiles
`loop_executor.hpp` standalone); 4 test cases pass.

Mutation check: in `LoopExecutor::post`, remove the `try`/`catch` and call `task();` bare. Expected: FAIL in "a
throwing task is logged, and later tasks still run" (the exception leaves `runUntilIdle()`). Restore.

Then prove the header guard covers `include/morph/tui/`: remove `include/morph/tui/loop_executor.hpp` from the
`FILE_SET` list and re-configure. Expected: configure FAILS listing `include/morph/tui/loop_executor.hpp` as a
public header in no FILE_SET. Restore.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt include/morph/tui/loop_executor.hpp src/tui/loop_executor.cpp tests/tui
git commit -m "wip(tui): MORPH_BUILD_TUI, the morph_tui target and LoopExecutor

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: Install, presets, CI and the compiled-component rule

**Files:**
- Modify: `CMakeLists.txt` — the "Install & export" section: a block after the `if(TARGET morph_qt_impl)` block, and
  the `write_basic_package_version_file` call with the comment above it. The line
  `foreach(_morph_component IN ITEMS offline_sqlite qt_forms qt net)` stays byte-identical:
  `scripts/test_check_install_export.sh:252` sed-matches it.
- Modify: `cmake/morphConfig.cmake.in` — a block before `include("${CMAKE_CURRENT_LIST_DIR}/morphTargets.cmake")`
- Modify: `CMakePresets.json` — `windows-everything` and `linux-everything` `cacheVariables`
- Modify: `.github/workflows/ci.yml` — the Configure steps of `linux-sanitizers`
  (`cmake --preset ${{ matrix.preset }}
  -DMORPH_BUILD_NET=ON -DMORPH_BUILD_OFFLINE_SQLITE=ON …`, near line 608), `linux-all-features` (near line 2450) and
  `clang-tidy` (near line 3034)
- Modify: `CONTRIBUTING.md` — "Toolchain", first paragraph
- Modify: `README.md` — the "Relevant CMake options" sentence and the "The components are …" sentence

**Interfaces:**
- Consumes: `morph_tui`, `MORPH_LIBUNICODE_VERSION` (Task 5).
- Produces: install component `tui` (export name `morph::tui`), listed in `MORPH_INSTALLED_COMPONENTS`.

- [ ] **Step 1: Install and export the component**

In `CMakeLists.txt`, after the `if(TARGET morph_qt_impl) … endif()` block of the install section, add:

```cmake
    # morph::tui is a compiled archive, like morph_qt_impl: it installs its
    # headers and its library. It links core::tui, which core-cpp installs only
    # when libunicode was found rather than fetched -- a fetched dependency is in
    # no export set, so core-cpp leaves core::tui out of its package and says
    # why. Exporting morph::tui then would name a target no installed package
    # provides, so it is left out too, and says so.
    if(TARGET morph_tui)
        get_target_property(_morph_core_tui_imported core::tui IMPORTED)
        get_target_property(_morph_unicode_imported unicode::unicode IMPORTED)
        if(_morph_core_tui_imported OR _morph_unicode_imported)
            set_target_properties(morph_tui PROPERTIES EXPORT_NAME tui)
            install(TARGETS morph_tui
                EXPORT morphTargets
                FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
                ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            )
            list(APPEND MORPH_INSTALLED_COMPONENTS tui)
        else()
            message(STATUS
                "morph: the tui component is not installed: libunicode was fetched, so core-cpp cannot "
                "install core::tui for it to link. Install libunicode ${MORPH_LIBUNICODE_VERSION} and put "
                "it on CMAKE_PREFIX_PATH to install morph::tui.")
        endif()
        unset(_morph_core_tui_imported)
        unset(_morph_unicode_imported)
    endif()
```

Replace the comment above `write_basic_package_version_file` and the call with:

```cmake
    # SameMajorVersion follows docs/spec/VERSIONING.md, which is what decides
    # this: there, minor releases are "additive and source-compatible" and
    # only a major breaks the stable surface, so an install of 0.2 does
    # satisfy a consumer asking for 0.1. ARCH_INDEPENDENT only while every
    # installed component is header-only: without it CMake rejects an install
    # made on a differently-sized-void* machine, which is exactly right once a
    # compiled archive (morph_qt_impl, the MorphForms module, morph::tui) is in
    # the package.
    set(_morph_arch_independent ARCH_INDEPENDENT)
    if(TARGET morph_qt_impl OR "forms_qml" IN_LIST MORPH_INSTALLED_COMPONENTS
       OR "tui" IN_LIST MORPH_INSTALLED_COMPONENTS)
        set(_morph_arch_independent "")
    endif()
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/morphConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
        ${_morph_arch_independent}
    )
    unset(_morph_arch_independent)
```

In `cmake/morphConfig.cmake.in`, directly before `include("${CMAKE_CURRENT_LIST_DIR}/morphTargets.cmake")`, add:

```cmake
# morph::tui links core::tui, which the core-cpp package found above provides
# only when it was installed with its TUI. Without it morphTargets.cmake would
# name a target nobody created.
if("tui" IN_LIST morph_KNOWN_COMPONENTS AND NOT TARGET core::tui)
    set(morph_FOUND FALSE)
    set(morph_NOT_FOUND_MESSAGE
        "this morph install has the tui component, but the core-cpp package found at '${core-cpp_DIR}' "
        "provides no core::tui.")
    return()
endif()
```

- [ ] **Step 2: Presets, CI and the documented rule**

`CMakePresets.json`: add `"MORPH_BUILD_TUI": "ON",` after `"MORPH_BUILD_QT": "ON",` in both `windows-everything` and
`linux-everything`.

`.github/workflows/ci.yml`: in each of the three Configure steps named above, add the line `-DMORPH_BUILD_TUI=ON \`
directly after `-DMORPH_BUILD_NET=ON \`. In `linux-sanitizers`, extend the comment above its Configure step with:
"morph::tui is built too: its event pump, its posted redraws and its loop executor are where a terminal
application's threading lives."

`CONTRIBUTING.md`, "Toolchain": after "morph is a header-only C++23 library." insert:

```markdown
One kind of component is compiled: an optional frontend that wraps a compiled
toolkit — `morph::tui` (`MORPH_BUILD_TUI`) over core-cpp's `core::tui`, like
`morph_qt_impl` for Qt's MOC — is a static library, so its widgets compile once
rather than in every consumer.
```

`README.md`: in "Relevant CMake options: …" add `` `MORPH_BUILD_TUI`, `` after `` `MORPH_BUILD_QT`, ``; replace
"The components are `net` (`MORPH_BUILD_NET`, POSIX only), `offline_sqlite`" with "The components are `net`
(`MORPH_BUILD_NET`, POSIX only), `tui` (`MORPH_BUILD_TUI`; installed when libunicode was found rather than
fetched), `offline_sqlite`".

- [ ] **Step 3: Verify the install, both ways it can go**

```bash
cmake -S . -B build/install-tui -G Ninja -DCMAKE_BUILD_TYPE=Release -DMORPH_BUILD_TESTS=OFF \
      -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_TUI=ON 2>&1 | tee build/install-tui.log
cmake --build build/install-tui
cmake --install build/install-tui --prefix "$PWD/build/install-tui/prefix"
grep -c "morph: the tui component is not installed" build/install-tui.log
```

- **libunicode was fetched** (the grep prints 1): `cmake --install` exits 0,
  `prefix/lib/cmake/morph/morphTargets.cmake`
  names no `morph::tui`, and `morphConfig.cmake`'s `morph_KNOWN_COMPONENTS` has no `tui`. Mutation: replace the
  `if(_morph_core_tui_imported OR _morph_unicode_imported)` condition with `if(TRUE)` and re-configure. Expected:
  generation FAILS — `install(EXPORT "morphTargets" ...) includes target "morph_tui" which requires target
  "core-cpp-tui" that is not in any export set`. Restore.
- **libunicode was found** (the grep prints 0): `morphTargets.cmake` contains `morph::tui`, and a consumer with
  `find_package(morph CONFIG REQUIRED COMPONENTS tui)` and `target_link_libraries(app PRIVATE morph::tui)` configures
  against the prefix. Mutation: delete `list(APPEND MORPH_INSTALLED_COMPONENTS tui)`, re-install. Expected: that
  consumer FAILS with "morph component 'tui' is not part of this install". Restore.

State in the hand-off which branch this machine took; the other is inferred from the code, not measured.

Then: `grep -c CMAKE_SIZEOF_VOID_P build/install-tui/morphConfigVersion.cmake` prints 1 when the tui component was
installed and 0 when it was not. And run the existing gates, which must still pass untouched:

```bash
bash scripts/check_install_export.sh
bash scripts/test_check_install_export.sh
```

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt cmake/morphConfig.cmake.in CMakePresets.json .github/workflows/ci.yml CONTRIBUTING.md README.md
git commit -m "wip(tui): install the tui component, and build it in the everything presets and CI

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The layout solver

**Files:**
- Create: `src/tui/layout.hpp`, `src/tui/layout.cpp`
- Modify: `CMakeLists.txt` — `add_library(morph_tui STATIC …)`: add `src/tui/layout.cpp` and `src/tui/layout.hpp`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_layout.cpp`
- Test: `tests/tui/test_tui_layout.cpp`

**Interfaces:**
- Consumes: `ui::Sizing` (`Kind::{Content, Fixed, Stretch}`, `amount`, `content()`, `fixed(int)`, `stretch(int)`)
  from `include/morph/ui/view.hpp` (Part 2); `core::tui::Rect` (core-cpp `tui/Rect.hpp`).
- Produces (`namespace morph::tui::layout`): `Item{sizing, natural}`, `Track{length, gap}`,
  `distribute(std::span<Item const>, Track) -> std::vector<int>`, `crossExtent(ui::Sizing, int space) -> int`,
  `GridCell{span, naturalHeight, height}`, `GridSlot{column, row, span}`,
  `GridPlan{slots, columnX, columnWidths, rowY, rowHeights, gap}`,
  `planGrid(int columns, std::span<GridCell const>, Track width) -> GridPlan`,
  `slotArea(GridPlan const&, GridSlot const&) -> core::tui::Rect`, `pad(core::tui::Rect, int) -> core::tui::Rect`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_layout.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <core/tui/Rect.hpp>
#include <morph/ui/view.hpp>
#include <string_view>
#include <vector>

#include "tui/layout.hpp"

namespace layout = morph::tui::layout;
using morph::ui::Sizing;

namespace {

struct DistributeCase {
    std::string_view name;
    std::vector<layout::Item> items;
    layout::Track track;
    std::vector<int> expected;
};

struct GridCase {
    std::string_view name;
    int columns = 1;
    std::vector<layout::GridCell> cells;
    layout::Track width;
    std::vector<layout::GridSlot> slots;
    std::vector<int> rowHeights;
};

}  // namespace

TEST_CASE("tui::layout::distribute", "[tui][layout]") {
    auto const testCase = GENERATE(values<DistributeCase>({
        {"fixed, content and stretch",
         {{Sizing::fixed(3), 0}, {Sizing::content(), 2}, {Sizing::stretch(1), 9}},
         {.length = 10, .gap = 0},
         {3, 2, 5}},
        {"equal weights: the remainder goes to the first", {{Sizing::stretch(1), 0}, {Sizing::stretch(1), 0}, {Sizing::stretch(1), 0}}, {.length = 10, .gap = 0}, {4, 3, 3}},
        {"weights and a gap", {{Sizing::stretch(2), 0}, {Sizing::stretch(1), 0}}, {.length = 10, .gap = 1}, {6, 3}},
        {"uneven weights: the remainder still goes to the first", {{Sizing::stretch(1), 0}, {Sizing::stretch(2), 0}}, {.length = 10, .gap = 0}, {4, 6}},
        {"overflow shrinks the last item first", {{Sizing::fixed(6), 0}, {Sizing::fixed(6), 0}}, {.length = 10, .gap = 0}, {6, 4}},
        {"overflow past one item reaches the one before", {{Sizing::content(), 3}, {Sizing::fixed(4), 0}, {Sizing::content(), 5}}, {.length = 8, .gap = 1}, {3, 3, 0}},
        {"no items", {}, {.length = 10, .gap = 0}, {}},
        {"stretch into nothing", {{Sizing::stretch(1), 4}}, {.length = 0, .gap = 0}, {0}},
        {"a negative fixed size is zero", {{Sizing::fixed(-2), 0}}, {.length = 5, .gap = 0}, {0}},
        {"a zero weight counts as one", {{Sizing::stretch(0), 0}, {Sizing::stretch(1), 0}}, {.length = 4, .gap = 0}, {2, 2}},
    }));
    INFO(testCase.name);
    CHECK(layout::distribute(testCase.items, testCase.track) == testCase.expected);
}

TEST_CASE("tui::layout::crossExtent", "[tui][layout]") {
    CHECK(layout::crossExtent(Sizing::fixed(4), 10) == 4);
    CHECK(layout::crossExtent(Sizing::fixed(12), 10) == 10);
    CHECK(layout::crossExtent(Sizing::fixed(-1), 10) == 0);
    CHECK(layout::crossExtent(Sizing::content(), 10) == 10);
    CHECK(layout::crossExtent(Sizing::stretch(3), 10) == 10);
}

TEST_CASE("tui::layout::planGrid", "[tui][layout]") {
    auto const testCase = GENERATE(values<GridCase>({
        {"two columns, a full-width third cell",
         2,
         {{.span = 1, .naturalHeight = 1, .height = {}}, {.span = 1, .naturalHeight = 1, .height = {}}, {.span = 2, .naturalHeight = 1, .height = {}}},
         {.length = 21, .gap = 1},
         {{.column = 0, .row = 0, .span = 1}, {.column = 1, .row = 0, .span = 1}, {.column = 0, .row = 1, .span = 2}},
         {1, 1}},
        {"a span that does not fit the rest of the row starts the next",
         2,
         {{.span = 1, .naturalHeight = 1, .height = {}}, {.span = 2, .naturalHeight = 1, .height = {}}},
         {.length = 20, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}, {.column = 0, .row = 1, .span = 2}},
         {1, 1}},
        {"a span wider than the grid is clamped",
         3,
         {{.span = 5, .naturalHeight = 2, .height = {}}},
         {.length = 9, .gap = 0},
         {{.column = 0, .row = 0, .span = 3}},
         {2}},
        {"a row is as tall as its tallest cell; Fixed height wins over natural",
         2,
         {{.span = 1, .naturalHeight = 1, .height = {}}, {.span = 1, .naturalHeight = 3, .height = {}}, {.span = 1, .naturalHeight = 5, .height = Sizing::fixed(2)}},
         {.length = 10, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}, {.column = 1, .row = 0, .span = 1}, {.column = 0, .row = 1, .span = 1}},
         {3, 2}},
        {"zero columns are one", 0, {{.span = 1, .naturalHeight = 1, .height = {}}}, {.length = 4, .gap = 0}, {{.column = 0, .row = 0, .span = 1}}, {1}},
    }));
    INFO(testCase.name);
    auto const plan = layout::planGrid(testCase.columns, testCase.cells, testCase.width);
    CHECK(plan.slots == testCase.slots);
    CHECK(plan.rowHeights == testCase.rowHeights);
}

TEST_CASE("tui::layout::slotArea spans columns and the gaps between them", "[tui][layout]") {
    std::vector<layout::GridCell> const cells{{.span = 1, .naturalHeight = 1, .height = {}},
                                              {.span = 1, .naturalHeight = 1, .height = {}},
                                              {.span = 2, .naturalHeight = 1, .height = {}}};
    auto const plan = layout::planGrid(2, cells, {.length = 21, .gap = 1});
    CHECK(plan.columnX == std::vector<int>{0, 11});
    CHECK(plan.columnWidths == std::vector<int>{10, 10});
    CHECK(plan.rowY == std::vector<int>{0, 2});
    CHECK(layout::slotArea(plan, plan.slots.at(1)) == core::tui::Rect{.x = 11, .y = 0, .width = 10, .height = 1});
    CHECK(layout::slotArea(plan, plan.slots.at(2)) == core::tui::Rect{.x = 0, .y = 2, .width = 21, .height = 1});
}

TEST_CASE("tui::layout::pad", "[tui][layout]") {
    CHECK(layout::pad({.x = 0, .y = 0, .width = 10, .height = 6}, 1) ==
          core::tui::Rect{.x = 1, .y = 1, .width = 8, .height = 4});
    CHECK(layout::pad({.x = 2, .y = 2, .width = 3, .height = 3}, 2) ==
          core::tui::Rect{.x = 4, .y = 4, .width = 0, .height = 0});
}
```

Add `test_tui_layout.cpp` to `add_executable(morph_tui_tests …)` in `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'tui/layout.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/tui/layout.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Rect.hpp>
#include <morph/ui/view.hpp>
#include <span>
#include <vector>

/// The stack and grid solver the TUI containers arrange their children with, in terminal cells. Specified in
/// docs/spec/tui/frontend.md, "Layout".
namespace morph::tui::layout {

/// One child along a stack's main axis.
struct Item {
    ui::Sizing sizing{};  ///< How the child asks for space.
    int natural = 0;      ///< Its content extent along the axis.
};

/// The space along one axis and the gap between two neighbours on it.
struct Track {
    int length = 0;  ///< Cells available.
    int gap = 0;     ///< Cells between two neighbours.
};

/// Extents along a stack's main axis, in item order. Fixed items get their amount and Content items their natural
/// extent; Stretch items share what is left by weight (a weight below one counts as one), the cells a division
/// leaves over going one each to the first Stretch items; an overflow is taken from the last item first.
[[nodiscard]] std::vector<int> distribute(std::span<Item const> items, Track track);

/// A child's extent across a stack: a Fixed child's amount, at most `space`; Content and Stretch fill `space`.
[[nodiscard]] int crossExtent(ui::Sizing sizing, int space);

/// What one grid cell asks for.
struct GridCell {
    int span = 1;           ///< Columns it covers; clamped to [1, columns].
    int naturalHeight = 1;  ///< Its content height.
    ui::Sizing height{};    ///< Fixed overrides the natural height; Content and Stretch use it.
};

/// Where one cell lands.
struct GridSlot {
    int column = 0;  ///< First column.
    int row = 0;     ///< Row.
    int span = 1;    ///< Columns covered.

    bool operator==(GridSlot const&) const = default;
};

/// A solved grid: one slot per cell, in cell order, and the geometry of its columns and rows.
struct GridPlan {
    std::vector<GridSlot> slots;      ///< One per cell.
    std::vector<int> columnX;         ///< Left edge of each column.
    std::vector<int> columnWidths;    ///< Width of each column.
    std::vector<int> rowY;            ///< Top edge of each row.
    std::vector<int> rowHeights;      ///< Height of each row.
    int gap = 0;                      ///< Cells between columns and between rows.
};

/// Places cells row-major in `columns` equal columns (fewer than one counts as one). A cell whose span does not fit
/// the rest of its row starts the next row; a row is as tall as its tallest cell.
[[nodiscard]] GridPlan planGrid(int columns, std::span<GridCell const> cells, Track width);

/// The rectangle a slot of `plan` covers, gaps between its columns included.
[[nodiscard]] ::core::tui::Rect slotArea(GridPlan const& plan, GridSlot const& slot);

/// `area` shrunk by `padding` cells on every side; never negative.
[[nodiscard]] ::core::tui::Rect pad(::core::tui::Rect area, int padding);

}  // namespace morph::tui::layout
```

Create `src/tui/layout.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/layout.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <ranges>

namespace morph::tui::layout {

namespace {

bool isStretch(ui::Sizing const& sizing) noexcept {
    return sizing.kind == ui::Sizing::Kind::Stretch;
}

int weightOf(ui::Sizing const& sizing) noexcept {
    return std::max(1, sizing.amount);
}

int gapsBetween(std::size_t count, int gap) noexcept {
    return count == 0 ? 0 : gap * (static_cast<int>(count) - 1);
}

}  // namespace

std::vector<int> distribute(std::span<Item const> items, Track track) {
    std::vector<int> sizes;
    sizes.reserve(items.size());
    int used = 0;
    int weights = 0;
    for (auto const& item : items) {
        if (isStretch(item.sizing)) {
            weights += weightOf(item.sizing);
            sizes.push_back(0);
            continue;
        }
        int const size = std::max(0, item.sizing.kind == ui::Sizing::Kind::Fixed ? item.sizing.amount : item.natural);
        sizes.push_back(size);
        used += size;
    }
    int const spare = track.length - gapsBetween(items.size(), track.gap) - used;
    if (spare > 0 && weights > 0) {
        int given = 0;
        auto size = sizes.begin();
        for (auto const& item : items) {
            if (isStretch(item.sizing)) {
                *size = spare * weightOf(item.sizing) / weights;
                given += *size;
            }
            ++size;
        }
        // The cells the division left over go one each to the first Stretch items.
        int remainder = spare - given;
        size = sizes.begin();
        for (auto const& item : items) {
            if (remainder == 0) {
                break;
            }
            if (isStretch(item.sizing)) {
                ++*size;
                --remainder;
            }
            ++size;
        }
    } else if (spare < 0) {
        // An overflow is taken from the last item first.
        int deficit = -spare;
        for (auto& size : sizes | std::views::reverse) {
            int const take = std::min(size, deficit);
            size -= take;
            deficit -= take;
        }
    }
    return sizes;
}

int crossExtent(ui::Sizing sizing, int space) {
    if (sizing.kind == ui::Sizing::Kind::Fixed) {
        return std::clamp(sizing.amount, 0, std::max(0, space));
    }
    return std::max(0, space);
}

GridPlan planGrid(int columns, std::span<GridCell const> cells, Track width) {
    int const count = std::max(1, columns);
    GridPlan plan;
    plan.gap = width.gap;
    std::vector<Item> const equal(static_cast<std::size_t>(count), Item{.sizing = ui::Sizing::stretch(1), .natural = 0});
    plan.columnWidths = distribute(equal, width);
    int x = 0;
    for (int const columnWidth : plan.columnWidths) {
        plan.columnX.push_back(x);
        x += columnWidth + width.gap;
    }

    int column = 0;
    int row = 0;
    for (auto const& cell : cells) {
        int const span = std::clamp(cell.span, 1, count);
        if (column + span > count) {
            ++row;
            column = 0;
        }
        plan.slots.push_back(GridSlot{.column = column, .row = row, .span = span});
        int const height = cell.height.kind == ui::Sizing::Kind::Fixed ? std::max(0, cell.height.amount)
                                                                        : std::max(0, cell.naturalHeight);
        if (static_cast<int>(plan.rowHeights.size()) <= row) {
            plan.rowHeights.push_back(height);
        } else {
            plan.rowHeights.back() = std::max(plan.rowHeights.back(), height);
        }
        column += span;
    }
    int y = 0;
    for (int const rowHeight : plan.rowHeights) {
        plan.rowY.push_back(y);
        y += rowHeight + width.gap;
    }
    return plan;
}

::core::tui::Rect slotArea(GridPlan const& plan, GridSlot const& slot) {
    auto const first = static_cast<std::size_t>(slot.column);
    auto const spanned = plan.columnWidths | std::views::drop(first) | std::views::take(static_cast<std::size_t>(slot.span));
    int const width = std::accumulate(spanned.begin(), spanned.end(), 0) + plan.gap * (slot.span - 1);
    auto const row = static_cast<std::size_t>(slot.row);
    return ::core::tui::Rect{.x = plan.columnX.at(first),
                             .y = plan.rowY.at(row),
                             .width = width,
                             .height = plan.rowHeights.at(row)};
}

::core::tui::Rect pad(::core::tui::Rect area, int padding) {
    return ::core::tui::Rect{.x = area.x + padding,
                             .y = area.y + padding,
                             .width = std::max(0, area.width - (2 * padding)),
                             .height = std::max(0, area.height - (2 * padding))};
}

}  // namespace morph::tui::layout
```

Add `src/tui/layout.cpp` and `src/tui/layout.hpp` to `add_library(morph_tui STATIC …)` in `CMakeLists.txt`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[layout]"`
Expected: PASS, 5 test cases.

Mutation check: in `distribute`, delete the remainder loop (the one after "The cells the division left over").
Expected: FAIL in "equal weights: the remainder goes to the first" (`{3, 3, 3}`) and "uneven weights: the remainder
still goes to the first" (`{3, 6}`). Restore.

- [ ] **Step 5: Commit**

```bash
git add src/tui/layout.hpp src/tui/layout.cpp tests/tui CMakeLists.txt
git commit -m "wip(tui): the stack and grid layout solver

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 8: The widget base, stacks and the first leaf widgets

**Files:**
- Create: `src/tui/context.hpp`, `src/tui/context.cpp`, `src/tui/widget.hpp`, `src/tui/widget.cpp`
- Create: `src/tui/leaf_widgets.hpp`, `src/tui/leaf_widgets.cpp` (Text, Button, Checkbox, Spacer)
- Create: `src/tui/container_widgets.hpp`, `src/tui/container_widgets.cpp` (Stack, Slot)
- Create: `tests/tui/tui_harness.hpp`
- Modify: `CMakeLists.txt` — `add_library(morph_tui STATIC …)`: add the six `src/tui/` files above
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_widgets.cpp`
- Test: `tests/tui/test_tui_widgets.cpp`

**Interfaces:**
- Consumes: `ui::Widget`, `ui::ContainerWidget`, `ui::TextWidget`, `ui::ButtonWidget`, `ui::CheckboxWidget`,
  `ui::SpacerWidget`, `ui::StackWidget`, `ui::SlotWidget`, `ui::LayoutHints`, `ui::Sizing`, `ui::Key`, `ui::Axis`,
  `ui::TextRole`, `ui::Action` (Part 2, `include/morph/ui/{view,backend}.hpp`); `layout::*` (Task 7);
  `core::tui::{Component, LayoutParams, Screen, Canvas, Theme, InputEvent, KeyEvent, MouseEvent, EventResult, Point,
  Rect, Size, stringWidth, withoutLockKeys}` (core-cpp 0.7).
- Produces (`namespace morph::tui::detail`, private):
  - `enum class Direction { Forward, Backward }`; `struct Context { screen, owners, roots, openDialogs, activeBusy,
    animationFrame; ownerOf(Component const*), forget(WidgetBase&) }`.
  - `class WidgetBase` — `of(ui::Widget&)`, `view()`, `context()`, `container()`, `asWidget()`, `applyVisible`,
    `setStructuralVisible`, `userVisible()`, `shown()`, `applyEnabled`, `enabled()`, `applyLayout`, `layout()`,
    `applyDragKey`, `dragKey()`, `applyDropHandler`, `isDropTarget()`, `accepts(Key)`, `drop(Key)`, virtual
    `naturalSize()`, `wantsFocus()`, `expandsByDefault()`, `probeText()`, `paint(Canvas&)`, `key(KeyEvent)`,
    `activate()`, `click(Point)`, `wheel(int)`; `dispatch(InputEvent)`, `pointer(MouseEvent)`, `focusable()`,
    `hasFocus()`, `refresh()`; protected `adopt(std::unique_ptr<V>)`.
  - `class ContainerBase : WidgetBase` — `of(ui::ContainerWidget&)`, `attach`, `forget`, `move`, `children()`,
    `shownChildren()`, virtual `host()`, protected `childAttached`, `childForgotten`.
  - `template <class Base> class Hosted`, `class View`, `template <class I> class TuiWidget`,
    `template <class I> class TuiContainer`.
  - `struct StackSpec { axis, gap, skip, extents }`; `stackNaturalSize`, `arrangeStack`, `splitLines`,
    `displayWidth`, `isActivation`, `collectFocusable`, `isWithin`, `attach(Context&, ui::ContainerWidget*,
    WidgetBase&)`, `fit(Context&)`, `moveFocus(Context&, Direction)`.
  - Widgets `TextImpl`, `ButtonImpl`, `CheckboxImpl`, `SpacerImpl`, `StackImpl` (with `axis()`, `gap()`,
    `setSkip`, `skip()`, `setColumnLayout(std::vector<int>, int)`), `SlotImpl`.
  - Test helper `morph::tui::testing::{rowsOf, ResizableOutput, Harness}`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/tui_harness.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TerminalOutput.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/VtParser.hpp>
#include <cstddef>
#include <memory>
#include <morph/ui/backend.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/context.hpp"
#include "tui/widget.hpp"

namespace morph::tui::testing {

/// The rows of the screen's last frame, trailing blanks removed and trailing empty rows dropped.
inline std::vector<std::string> rowsOf(::core::tui::Screen const& screen) {
    auto const text = ::core::tui::test::canvasToString(screen.renderedBuffer());
    std::vector<std::string> rows;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto const end = std::min(text.find('\n', start), text.size());
        auto row = text.substr(start, end - start);
        while (!row.empty() && row.back() == ' ') {
            row.pop_back();
        }
        rows.push_back(std::move(row));
        start = end + 1;
    }
    while (!rows.empty() && rows.back().empty()) {
        rows.pop_back();
    }
    return rows;
}

/// A mock terminal whose size a test can change, and which counts the frames drawn to it.
class ResizableOutput final : public ::core::tui::MockTerminalOutput {
public:
    explicit ResizableOutput(::core::tui::Size size) : MockTerminalOutput{size.width, size.height}, _size{size} {}
    [[nodiscard]] auto columns() const noexcept -> int override { return _size.width; }
    [[nodiscard]] auto rows() const noexcept -> int override { return _size.height; }
    void resize(::core::tui::Size size) noexcept { _size = size; }
    /// One synchronised update per `Screen::draw()` in fullscreen mode, so this counts frames.
    [[nodiscard]] auto syncGuard() -> ::core::tui::SyncGuard override {
        ++_frames;
        return MockTerminalOutput::syncGuard();
    }
    [[nodiscard]] int frames() const noexcept { return _frames; }

private:
    ::core::tui::Size _size;
    int _frames = 0;
};

/// A headless screen and the widget context of one backend over it.
class Harness {
public:
    explicit Harness(int columns = 40, int rows = 12)
        : Harness{std::make_unique<::core::tui::MockTerminalOutput>(columns, rows)} {}
    explicit Harness(std::unique_ptr<::core::tui::TerminalOutput> output)
        : _terminal{std::move(output)}, _screen{_terminal}, _context{_screen} {}

    /// Creates a widget and attaches it under @p parent (null: the screen's root), as a backend factory does.
    template <class Widget, class... Args>
    std::unique_ptr<Widget> make(ui::ContainerWidget* parent, Args&&... args) {
        auto widget = std::make_unique<Widget>(_context, std::forward<Args>(args)...);
        detail::attach(_context, parent, *widget);
        return widget;
    }

    /// Fits the roots to the screen, draws a frame and returns its rows.
    std::vector<std::string> draw() {
        detail::fit(_context);
        _screen.draw();
        return rowsOf(_screen);
    }

    ::core::tui::EventResult send(::core::tui::InputEvent const& event) { return _screen.dispatchEvent(event); }

    ::core::tui::EventResult key(::core::tui::KeyCode code,
                                 ::core::tui::Modifier modifiers = ::core::tui::Modifier::None) {
        return send(::core::tui::test::specialKey(code, modifiers));
    }

    /// Sends one key event per character of @p text; returns the last result.
    ::core::tui::EventResult type(std::string_view text) {
        auto result = ::core::tui::EventResult::Ignored;
        for (char const character : text) {
            result = send(::core::tui::test::charKey(character));
        }
        return result;
    }

    /// A left-button press and release on a 0-based viewport cell; returns the release's result.
    ::core::tui::EventResult click(::core::tui::Point cell) {
        using Type = ::core::tui::MouseEvent::Type;
        static_cast<void>(send(::core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = cell.x + 1, .y = cell.y + 1}));
        return send(::core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = cell.x + 1, .y = cell.y + 1});
    }

    /// Decodes @p bytes as terminal input (SGR mouse reports included) and dispatches each event.
    void sgr(std::string_view bytes) {
        ::core::tui::VtParser parser;
        for (auto const& event : parser.feed(bytes)) {
            static_cast<void>(_screen.dispatchEvent(event));
        }
    }

    void focus(ui::Widget const& widget) { _screen.setFocus(&detail::WidgetBase::of(widget).view()); }

    [[nodiscard]] bool focused(ui::Widget const& widget) const {
        return _screen.focusedComponent() == &detail::WidgetBase::of(widget).view();
    }

    [[nodiscard]] ::core::tui::Screen& screen() noexcept { return _screen; }
    [[nodiscard]] detail::Context& context() noexcept { return _context; }

private:
    ::core::tui::Terminal _terminal;
    ::core::tui::Screen _screen;
    detail::Context _context;
};

}  // namespace morph::tui::testing
```

Create `tests/tui/test_tui_widgets.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::CheckboxImpl;
using morph::tui::detail::Direction;
using morph::tui::detail::SpacerImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

TEST_CASE("tui widgets: a column renders its children top to bottom", "[tui][widgets]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const title = harness.make<TextImpl>(column.get());
    title->setText("Title");
    auto const goButton = harness.make<ButtonImpl>(column.get());
    goButton->setLabel("Go");
    CHECK(harness.draw() == Rows{"Title", "[ Go ]"});
}

TEST_CASE("tui widgets: a row lays out left to right, with its gap", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    row->setGap(2);
    auto const left = harness.make<TextImpl>(row.get());
    left->setText("ab");
    auto const right = harness.make<TextImpl>(row.get());
    right->setText("cd");
    CHECK(harness.draw() == Rows{"ab  cd"});
}

TEST_CASE("tui widgets: a spacer takes the space its siblings leave", "[tui][widgets]") {
    Harness harness{20, 1};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    auto const left = harness.make<TextImpl>(row.get());
    left->setText("L");
    auto const spacer = harness.make<SpacerImpl>(row.get());
    auto const right = harness.make<TextImpl>(row.get());
    right->setText("R");
    CHECK(harness.draw() == Rows{"L" + std::string(18, ' ') + "R"});
}

TEST_CASE("tui widgets: a hidden child takes no space", "[tui][widgets]") {
    Harness harness{10, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const first = harness.make<TextImpl>(column.get());
    first->setText("a");
    auto const second = harness.make<TextImpl>(column.get());
    second->setText("b");
    auto const third = harness.make<TextImpl>(column.get());
    third->setText("c");
    second->setVisible(false);
    CHECK(harness.draw() == Rows{"a", "c"});
}

TEST_CASE("tui widgets: multi-line text takes one row per line", "[tui][widgets]") {
    Harness harness{10, 3};
    auto const text = harness.make<TextImpl>(nullptr);
    text->setText("one\ntwo");
    CHECK(harness.draw() == Rows{"one", "two"});
}

TEST_CASE("tui widgets: moveChild reorders both rendering and focus order", "[tui][widgets]") {
    Harness harness{10, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const first = harness.make<ButtonImpl>(column.get());
    first->setLabel("A");
    auto const second = harness.make<ButtonImpl>(column.get());
    second->setLabel("B");
    column->moveChild(*second, 0);
    CHECK(harness.draw() == Rows{"[ B ]", "[ A ]"});
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*second));
}

TEST_CASE("tui widgets: Enter, Space and a click activate a button; a disabled one ignores them", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const button = harness.make<ButtonImpl>(nullptr);
    int clicks = 0;
    button->setLabel("Go");
    button->setOnClick([&] { ++clicks; });
    harness.focus(*button);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(harness.type(" ") == EventResult::Handled);
    CHECK(clicks == 2);
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(clicks == 3);

    button->setEnabled(false);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    static_cast<void>(harness.click({.x = 1, .y = 0}));
    CHECK(clicks == 3);
}

TEST_CASE("tui widgets: a checkbox toggles on Enter and reports the new state; setChecked reports nothing",
          "[tui][widgets]") {
    Harness harness{20, 1};
    auto const box = harness.make<CheckboxImpl>(nullptr);
    std::vector<bool> toggles;
    box->setLabel("Done");
    box->setChecked(false);
    box->setOnToggle([&](bool checked) { toggles.push_back(checked); });
    CHECK(harness.draw() == Rows{"[ ] Done"});
    harness.focus(*box);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(toggles == std::vector<bool>{true});
    CHECK(harness.draw() == Rows{"[x] Done"});
    box->setChecked(false);
    CHECK(harness.draw() == Rows{"[ ] Done"});
    CHECK(toggles.size() == 1);
}

TEST_CASE("tui widgets: destroying the focused widget clears the screen's focus", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto button = harness.make<ButtonImpl>(column.get());
    button->setLabel("Go");
    harness.focus(*button);
    REQUIRE(harness.screen().focusedComponent() != nullptr);
    button.reset();
    CHECK(harness.screen().focusedComponent() == nullptr);
    CHECK(harness.draw().empty());
}
```

Add `test_tui_widgets.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'tui/context.hpp' file not found`.

- [ ] **Step 3: Implement the context and the widget base**

Create `src/tui/context.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Component.hpp>
#include <core/tui/Screen.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace morph::tui::detail {

class WidgetBase;

/// Which way keyboard focus moves.
enum class Direction : std::uint8_t { Forward, Backward };

/// What every widget of one backend shares: the screen, which widget owns which view, the roots, the open dialogs
/// and the spinner state.
struct Context {
    explicit Context(::core::tui::Screen& target);
    ~Context();
    Context(Context const&) = delete;
    Context& operator=(Context const&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    ::core::tui::Screen* screen;                                             ///< Where every view renders.
    std::unordered_map<::core::tui::Component const*, WidgetBase*> owners;  ///< Each registered view's widget.
    std::vector<WidgetBase*> roots;                                          ///< Widgets created with no parent.
    std::vector<::core::tui::Component*> openDialogs;                        ///< Open dialog frames, innermost last.
    std::size_t activeBusy = 0;                                              ///< Busy widgets that are spinning.
    std::size_t animationFrame = 0;                                          ///< The spinner frame to draw.

    /// The widget owning @p view, or null for a component no widget registered (an overlay, the root).
    [[nodiscard]] WidgetBase* ownerOf(::core::tui::Component const* view) const;
    /// Drops every reference to a widget being destroyed.
    void forget(WidgetBase& widget);
};

}  // namespace morph::tui::detail
```

Create `src/tui/context.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/context.hpp"

#include <vector>

namespace morph::tui::detail {

Context::Context(::core::tui::Screen& target) : screen{&target} {}

Context::~Context() = default;

WidgetBase* Context::ownerOf(::core::tui::Component const* view) const {
    auto const found = owners.find(view);
    return found == owners.end() ? nullptr : found->second;
}

void Context::forget(WidgetBase& widget) {
    std::erase(roots, &widget);
}

}  // namespace morph::tui::detail
```

Create `src/tui/widget.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/context.hpp"

namespace morph::tui::detail {

class ContainerBase;

/// The morph side of one TUI widget: the Common props, the container it sits in, and the core::tui view it owns.
///
/// The view is registered in the Context while the widget lives. The destructor clears the screen's focus from it
/// and unregisters it before the view itself is destroyed, so core::tui never keeps a pointer to a freed view.
class WidgetBase {
public:
    explicit WidgetBase(Context& context) noexcept;
    virtual ~WidgetBase();
    WidgetBase(WidgetBase const&) = delete;
    WidgetBase& operator=(WidgetBase const&) = delete;
    WidgetBase(WidgetBase&&) = delete;
    WidgetBase& operator=(WidgetBase&&) = delete;

    /// The TUI widget behind @p widget; throws std::logic_error for a widget another backend made.
    [[nodiscard]] static WidgetBase& of(ui::Widget& widget);
    /// The TUI widget behind @p widget; throws std::logic_error for a widget another backend made.
    [[nodiscard]] static WidgetBase const& of(ui::Widget const& widget);

    [[nodiscard]] ::core::tui::Component& view() const noexcept { return *_view; }
    [[nodiscard]] Context& context() const noexcept { return *_context; }
    [[nodiscard]] ContainerBase* container() const noexcept { return _container; }
    void setContainer(ContainerBase* container) noexcept { _container = container; }
    /// This widget as the ui interface it implements.
    [[nodiscard]] virtual ui::Widget const& asWidget() const noexcept = 0;

    void applyVisible(bool visible);
    /// Shown or hidden by its container (a collapsed panel), independently of `visible`.
    void setStructuralVisible(bool visible);
    [[nodiscard]] bool userVisible() const noexcept { return _userVisible; }
    [[nodiscard]] bool shown() const noexcept { return _userVisible && _structuralVisible; }
    void applyEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return _enabled; }
    void applyLayout(ui::LayoutHints const& hints);
    [[nodiscard]] ui::LayoutHints const& layout() const noexcept { return _layout; }
    void applyDragKey(std::optional<ui::Key> const& key);
    [[nodiscard]] std::optional<ui::Key> const& dragKey() const noexcept { return _dragKey; }
    void applyDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop);
    [[nodiscard]] bool isDropTarget() const noexcept { return static_cast<bool>(_onDrop); }
    /// Whether a drop of @p key lands here: a drop target whose `accepts` is empty or holds.
    [[nodiscard]] bool accepts(ui::Key const& key) const;
    void drop(ui::Key key) const;

    /// The size this widget asks for, in cells.
    [[nodiscard]] virtual ::core::tui::Size naturalSize() const = 0;
    /// Whether keyboard focus may land here (and, while enabled, does).
    [[nodiscard]] virtual bool wantsFocus() const { return false; }
    /// Whether a Content main-axis size means Stretch(1) in a stack (a Spacer).
    [[nodiscard]] virtual bool expandsByDefault() const { return false; }
    /// The text a conformance probe reads.
    [[nodiscard]] virtual std::string probeText() const { return {}; }
    /// Draws the widget into its own area; a container also places its children here.
    virtual void paint(::core::tui::Canvas& canvas);
    /// Handles a key while enabled and focused (or bubbled to).
    [[nodiscard]] virtual ::core::tui::EventResult key(::core::tui::KeyEvent const& key);
    /// The primary action: what Enter, Space and a click do.
    virtual void activate() {}
    /// A click on @p cell (0-based, relative to this widget); runs `activate()` unless overridden.
    virtual void click(::core::tui::Point cell);
    /// A wheel notch, -1 up and +1 down; true when handled.
    [[nodiscard]] virtual bool wheel(int delta);

    /// Routes one event of this widget's view: mouse to `pointer()`, keys to `key()` while enabled.
    [[nodiscard]] ::core::tui::EventResult dispatch(::core::tui::InputEvent const& event);
    /// Press, motion, release and wheel on this widget's view.
    [[nodiscard]] ::core::tui::EventResult pointer(::core::tui::MouseEvent const& mouse);
    [[nodiscard]] bool focusable() const { return wantsFocus() && _enabled; }
    [[nodiscard]] bool hasFocus() const noexcept { return _view != nullptr && _view->focused(); }
    /// Asks the screen for a full repaint on the next draw.
    void refresh() const;

protected:
    /// Takes ownership of the widget's view and registers it; returns it typed.
    template <class ViewType>
    ViewType& adopt(std::unique_ptr<ViewType> view) {
        ViewType& adopted = *view;
        _view = std::move(view);
        _context->owners.insert_or_assign(_view.get(), this);
        return adopted;
    }

private:
    void syncVisible();

    Context* _context;
    ContainerBase* _container = nullptr;
    bool _userVisible = true;
    bool _structuralVisible = true;
    bool _enabled = true;
    bool _pressed = false;
    ui::LayoutHints _layout{};
    std::optional<ui::Key> _dragKey;
    std::function<bool(ui::Key const&)> _accepts;
    std::function<void(ui::Key)> _onDrop;
    std::unique_ptr<::core::tui::Component> _view;
};

/// A widget whose children's views sit in a host component it provides (its own view, unless overridden).
class ContainerBase : public WidgetBase {
public:
    explicit ContainerBase(Context& context) noexcept : WidgetBase{context} {}
    ~ContainerBase() override;
    ContainerBase(ContainerBase const&) = delete;
    ContainerBase& operator=(ContainerBase const&) = delete;
    ContainerBase(ContainerBase&&) = delete;
    ContainerBase& operator=(ContainerBase&&) = delete;

    /// The TUI container behind @p container; throws std::logic_error for one another backend made.
    [[nodiscard]] static ContainerBase& of(ui::ContainerWidget& container);
    /// The TUI container behind @p container; throws std::logic_error for one another backend made.
    [[nodiscard]] static ContainerBase const& of(ui::ContainerWidget const& container);

    void attach(WidgetBase& child);
    void forget(WidgetBase& child);
    void move(WidgetBase& child, std::size_t index);
    [[nodiscard]] std::span<WidgetBase* const> children() const noexcept { return _children; }
    [[nodiscard]] std::vector<WidgetBase*> shownChildren() const;
    [[nodiscard]] virtual ::core::tui::Component& host() { return view(); }

protected:
    virtual void childAttached(WidgetBase& child);
    virtual void childForgotten(WidgetBase& child);

private:
    void restack();

    std::vector<WidgetBase*> _children;
};

/// A core::tui component that renders and reacts for its owner, and is visible only while its ancestors are.
template <class Base>
class Hosted : public Base {
public:
    explicit Hosted(WidgetBase& owner) : _owner{&owner} {}

    [[nodiscard]] bool visible() const noexcept override {
        return Base::visible() && (this->parent() == nullptr || this->parent()->visible());
    }
    [[nodiscard]] bool focusable() const override { return _owner->focusable(); }
    [[nodiscard]] ::core::tui::Size preferredSize() const override { return _owner->naturalSize(); }
    [[nodiscard]] WidgetBase& owner() const noexcept { return *_owner; }

private:
    WidgetBase* _owner;
};

/// The plain view most widgets use: painting and events go to the owner.
class View final : public Hosted<::core::tui::Component> {
public:
    using Hosted::Hosted;
    void render(::core::tui::Canvas& canvas) override { owner().paint(canvas); }
    [[nodiscard]] ::core::tui::EventResult onEvent(::core::tui::InputEvent const& event) override {
        return owner().dispatch(event);
    }
};

/// Implements the Common setters of ui interface @p Interface over WidgetBase.
template <class Interface>
class TuiWidget : public Interface, public WidgetBase {
public:
    explicit TuiWidget(Context& context) noexcept : WidgetBase{context} {}
    void setVisible(bool visible) override { applyVisible(visible); }
    void setEnabled(bool enabled) override { applyEnabled(enabled); }
    void setLayout(ui::LayoutHints const& hints) override { applyLayout(hints); }
    void setDragKey(std::optional<ui::Key> const& key) override { applyDragKey(key); }
    void setDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) override {
        applyDropHandler(std::move(accepts), std::move(onDrop));
    }
    [[nodiscard]] ui::Widget const& asWidget() const noexcept override { return *this; }
};

/// Implements the Common setters and `moveChild` of container interface @p Interface over ContainerBase.
template <class Interface>
class TuiContainer : public Interface, public ContainerBase {
public:
    explicit TuiContainer(Context& context) noexcept : ContainerBase{context} {}
    void setVisible(bool visible) override { applyVisible(visible); }
    void setEnabled(bool enabled) override { applyEnabled(enabled); }
    void setLayout(ui::LayoutHints const& hints) override { applyLayout(hints); }
    void setDragKey(std::optional<ui::Key> const& key) override { applyDragKey(key); }
    void setDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) override {
        applyDropHandler(std::move(accepts), std::move(onDrop));
    }
    void moveChild(ui::Widget& child, std::size_t index) override { move(WidgetBase::of(child), index); }
    [[nodiscard]] ui::Widget const& asWidget() const noexcept override { return *this; }
};

/// How `arrangeStack` places children.
struct StackSpec {
    ui::Axis axis = ui::Axis::Vertical;  ///< The main axis.
    int gap = 0;                         ///< Cells between neighbours.
    std::size_t skip = 0;                ///< Leading shown children given no area: scrolled out of view.
    std::span<int const> extents{};      ///< When non-empty, the main-axis extents to use, in order (table columns).
};

/// The natural size of @p children stacked along @p axis with @p gap between them; hidden children count nothing.
[[nodiscard]] ::core::tui::Size stackNaturalSize(std::span<WidgetBase* const> children, ui::Axis axis, int gap);
/// Places the shown @p children inside @p area (parent-relative) with the layout solver; hidden and skipped
/// children get an empty area, so they neither render nor take space but stay focusable.
void arrangeStack(std::span<WidgetBase* const> children, ::core::tui::Rect area, StackSpec const& spec);
/// @p text split at '\n'.
[[nodiscard]] std::vector<std::string_view> splitLines(std::string_view text);
/// The display width of @p text in cells.
[[nodiscard]] int displayWidth(std::string_view text);
/// Enter, or Space, with no modifier.
[[nodiscard]] bool isActivation(::core::tui::KeyEvent const& key) noexcept;
/// Appends every focusable, visible component below @p root, depth first.
void collectFocusable(::core::tui::Component& root, std::vector<::core::tui::Component*>& out);
/// Whether @p node is @p root or one of its descendants.
[[nodiscard]] bool isWithin(::core::tui::Component const* node, ::core::tui::Component const& root) noexcept;
/// Puts @p widget under @p parent, or at the screen's root when @p parent is null.
void attach(Context& context, ui::ContainerWidget* parent, WidgetBase& widget);
/// Sizes every root widget to the viewport.
void fit(Context& context);
/// Moves keyboard focus to the next or previous focusable component.
void moveFocus(Context& context, Direction direction);

}  // namespace morph::tui::detail
```

Create `src/tui/widget.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/widget.hpp"

#include <algorithm>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Unicode.hpp>
#include <iterator>
#include <stdexcept>
#include <variant>

#include "tui/layout.hpp"

namespace morph::tui::detail {

using ::core::tui::EventResult;

WidgetBase::WidgetBase(Context& context) noexcept : _context{&context} {}

// NOLINTNEXTLINE(bugprone-exception-escape): unregistering cannot stop half-way; leaving the screen pointing at a freed view would be worse than terminating.
WidgetBase::~WidgetBase() {
    if (_view != nullptr) {
        if (_context->screen->focusedComponent() == _view.get()) {
            _context->screen->setFocus(nullptr);
        }
        _context->owners.erase(_view.get());
    }
    _context->forget(*this);
    if (_container != nullptr) {
        _container->forget(*this);
    }
}

WidgetBase& WidgetBase::of(ui::Widget& widget) {
    auto* const base = dynamic_cast<WidgetBase*>(&widget);
    if (base == nullptr) {
        throw std::logic_error{"morph::tui: a widget another backend created"};
    }
    return *base;
}

WidgetBase const& WidgetBase::of(ui::Widget const& widget) {
    auto const* const base = dynamic_cast<WidgetBase const*>(&widget);
    if (base == nullptr) {
        throw std::logic_error{"morph::tui: a widget another backend created"};
    }
    return *base;
}

void WidgetBase::applyVisible(bool visible) {
    _userVisible = visible;
    syncVisible();
}

void WidgetBase::setStructuralVisible(bool visible) {
    _structuralVisible = visible;
    syncVisible();
}

void WidgetBase::syncVisible() {
    if (_view != nullptr) {
        _view->setVisible(shown());
    }
    refresh();
}

void WidgetBase::applyEnabled(bool enabled) {
    _enabled = enabled;
    refresh();
}

void WidgetBase::applyLayout(ui::LayoutHints const& hints) {
    _layout = hints;
    refresh();
}

void WidgetBase::applyDragKey(std::optional<ui::Key> const& key) {
    _dragKey = key;
}

void WidgetBase::applyDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) {
    _accepts = std::move(accepts);
    _onDrop = std::move(onDrop);
}

bool WidgetBase::accepts(ui::Key const& key) const {
    return isDropTarget() && (!_accepts || _accepts(key));
}

void WidgetBase::drop(ui::Key key) const {
    if (_onDrop) {
        _onDrop(std::move(key));
    }
}

void WidgetBase::paint(::core::tui::Canvas& /*canvas*/) {}

EventResult WidgetBase::key(::core::tui::KeyEvent const& /*key*/) {
    return EventResult::Ignored;
}

void WidgetBase::click(::core::tui::Point /*cell*/) {
    activate();
}

bool WidgetBase::wheel(int /*delta*/) {
    return false;
}

EventResult WidgetBase::dispatch(::core::tui::InputEvent const& event) {
    if (auto const* mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
        return pointer(*mouse);
    }
    if (auto const* pressed = std::get_if<::core::tui::KeyEvent>(&event); pressed != nullptr && _enabled) {
        return key(*pressed);
    }
    return EventResult::Ignored;
}

EventResult WidgetBase::pointer(::core::tui::MouseEvent const& mouse) {
    using Type = ::core::tui::MouseEvent::Type;
    if (mouse.type == Type::ScrollUp || mouse.type == Type::ScrollDown) {
        return wheel(mouse.type == Type::ScrollUp ? -1 : 1) ? EventResult::Handled : EventResult::Ignored;
    }
    ::core::tui::Point const cell{.x = mouse.x - 1, .y = mouse.y - 1};
    if (mouse.type == Type::Press) {
        if (mouse.button != 0 || !(focusable() || _dragKey)) {
            return EventResult::Ignored;
        }
        _pressed = true;
        if (focusable()) {
            _context->screen->setFocus(_view.get());
        }
        return EventResult::Handled;
    }
    if (!_pressed) {
        return EventResult::Ignored;
    }
    if (mouse.type == Type::Release) {
        _pressed = false;
        auto const bounds = _view->screenBounds();
        bool const inside = cell.x >= 0 && cell.y >= 0 && cell.x < bounds.width && cell.y < bounds.height;
        if (inside && _enabled) {
            click(cell);
        }
    }
    return EventResult::Handled;
}

void WidgetBase::refresh() const {
    _context->screen->invalidate();
}

ContainerBase::~ContainerBase() {
    for (auto* child : _children) {
        child->setContainer(nullptr);
    }
}

ContainerBase& ContainerBase::of(ui::ContainerWidget& container) {
    auto* const base = dynamic_cast<ContainerBase*>(&container);
    if (base == nullptr) {
        throw std::logic_error{"morph::tui: a container another backend created"};
    }
    return *base;
}

ContainerBase const& ContainerBase::of(ui::ContainerWidget const& container) {
    auto const* const base = dynamic_cast<ContainerBase const*>(&container);
    if (base == nullptr) {
        throw std::logic_error{"morph::tui: a container another backend created"};
    }
    return *base;
}

void ContainerBase::attach(WidgetBase& child) {
    _children.push_back(&child);
    child.setContainer(this);
    host().addChild(child.view(), ::core::tui::LayoutParams{.area = {}, .visible = child.shown()});
    childAttached(child);
    refresh();
}

void ContainerBase::forget(WidgetBase& child) {
    std::erase(_children, &child);
    childForgotten(child);
    refresh();
}

void ContainerBase::move(WidgetBase& child, std::size_t index) {
    auto const found = std::ranges::find(_children, &child);
    if (found == _children.end()) {
        return;
    }
    _children.erase(found);
    auto const position = static_cast<std::ptrdiff_t>(std::min(index, _children.size()));
    _children.insert(std::next(_children.begin(), position), &child);
    restack();
}

std::vector<WidgetBase*> ContainerBase::shownChildren() const {
    std::vector<WidgetBase*> shown;
    std::ranges::copy_if(_children, std::back_inserter(shown), [](WidgetBase const* child) { return child->shown(); });
    return shown;
}

void ContainerBase::childAttached(WidgetBase& /*child*/) {}

void ContainerBase::childForgotten(WidgetBase& /*child*/) {}

// The child views are re-added in order: core::tui's focus walk and paint order follow its child list.
void ContainerBase::restack() {
    auto& target = host();
    for (auto* child : _children) {
        target.removeChild(child->view());
    }
    for (auto* child : _children) {
        target.addChild(child->view(), ::core::tui::LayoutParams{.area = child->view().area(), .visible = child->shown()});
    }
    refresh();
}

::core::tui::Size stackNaturalSize(std::span<WidgetBase* const> children, ui::Axis axis, int gap) {
    bool const vertical = axis == ui::Axis::Vertical;
    int along = 0;
    int across = 0;
    int count = 0;
    for (auto const* child : children) {
        if (!child->shown()) {
            continue;
        }
        auto const natural = child->naturalSize();
        along += vertical ? natural.height : natural.width;
        across = std::max(across, vertical ? natural.width : natural.height);
        ++count;
    }
    if (count > 1) {
        along += gap * (count - 1);
    }
    return vertical ? ::core::tui::Size{.width = across, .height = along} : ::core::tui::Size{.width = along, .height = across};
}

void arrangeStack(std::span<WidgetBase* const> children, ::core::tui::Rect area, StackSpec const& spec) {
    bool const vertical = spec.axis == ui::Axis::Vertical;
    std::vector<WidgetBase*> placed;
    std::size_t skipped = 0;
    for (auto* child : children) {
        if (child->shown() && skipped >= spec.skip) {
            placed.push_back(child);
            continue;
        }
        if (child->shown()) {
            ++skipped;
        }
        child->view().setArea({});
    }

    std::vector<int> extents;
    if (!spec.extents.empty()) {
        auto given = spec.extents.begin();
        for (auto const* child : placed) {
            auto const natural = child->naturalSize();
            if (given != spec.extents.end()) {
                extents.push_back(*given);
                ++given;
            } else {
                extents.push_back(vertical ? natural.height : natural.width);
            }
        }
    } else {
        std::vector<layout::Item> items;
        for (auto const* child : placed) {
            auto sizing = vertical ? child->layout().height : child->layout().width;
            if (sizing.kind == ui::Sizing::Kind::Content && child->expandsByDefault()) {
                sizing = ui::Sizing::stretch(1);
            }
            auto const natural = child->naturalSize();
            items.push_back(layout::Item{.sizing = sizing, .natural = vertical ? natural.height : natural.width});
        }
        extents = layout::distribute(items, layout::Track{.length = vertical ? area.height : area.width, .gap = spec.gap});
    }

    int offset = 0;
    auto extent = extents.begin();
    for (auto* child : placed) {
        int const across = layout::crossExtent(vertical ? child->layout().width : child->layout().height,
                                               vertical ? area.width : area.height);
        child->view().setArea(vertical
                                  ? ::core::tui::Rect{.x = area.x, .y = area.y + offset, .width = across, .height = *extent}
                                  : ::core::tui::Rect{.x = area.x + offset, .y = area.y, .width = *extent, .height = across});
        offset += *extent + spec.gap;
        ++extent;
    }
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (;;) {
        auto const end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            return lines;
        }
        start = end + 1;
    }
}

int displayWidth(std::string_view text) {
    return ::core::tui::stringWidth(text);
}

bool isActivation(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None &&
           (key.key == ::core::tui::KeyCode::Enter || key.codepoint == U' ');
}

void collectFocusable(::core::tui::Component& root, std::vector<::core::tui::Component*>& out) {
    for (auto* child : root.children()) {
        if (child->focusable() && child->visible()) {
            out.push_back(child);
        }
        collectFocusable(*child, out);
    }
}

bool isWithin(::core::tui::Component const* node, ::core::tui::Component const& root) noexcept {
    for (; node != nullptr; node = node->parent()) {
        if (node == &root) {
            return true;
        }
    }
    return false;
}

void attach(Context& context, ui::ContainerWidget* parent, WidgetBase& widget) {
    if (parent == nullptr) {
        context.roots.push_back(&widget);
        context.screen->root().addChild(widget.view(), ::core::tui::LayoutParams{.area = context.screen->viewportArea()});
        return;
    }
    ContainerBase::of(*parent).attach(widget);
}

void fit(Context& context) {
    auto const area = context.screen->viewportArea();
    for (auto* root : context.roots) {
        root->view().setArea(area);
    }
}

void moveFocus(Context& context, Direction direction) {
    if (direction == Direction::Forward) {
        context.screen->focusNext();
    } else {
        context.screen->focusPrev();
    }
}

}  // namespace morph::tui::detail
```

- [ ] **Step 4: Implement the first leaf widgets and the stacks**

Create `src/tui/leaf_widgets.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <morph/ui/backend.hpp>
#include <string>
#include <string_view>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Text: one row per line, styled by role.
class TextImpl final : public TuiWidget<ui::TextWidget> {
public:
    explicit TextImpl(Context& context);
    void setText(std::string_view text) override;
    void setRole(ui::TextRole role) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::string probeText() const override { return _text; }
    void paint(::core::tui::Canvas& canvas) override;

private:
    std::string _text;
    ui::TextRole _role = ui::TextRole::Normal;
};

/// Button: `[ label ]`, activated by Enter, Space or a click.
class ButtonImpl final : public TuiWidget<ui::ButtonWidget> {
public:
    explicit ButtonImpl(Context& context);
    void setLabel(std::string_view label) override;
    void setOnClick(ui::Action onClick) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override { return _label; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;

private:
    std::string _label;
    ui::Action _onClick;
};

/// Checkbox: `[x] label`; Enter, Space or a click flips it and reports the new state.
class CheckboxImpl final : public TuiWidget<ui::CheckboxWidget> {
public:
    explicit CheckboxImpl(Context& context);
    void setLabel(std::string_view label) override;
    void setChecked(bool checked) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override { return _label; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;

private:
    std::string _label;
    bool _checked = false;
    std::function<void(bool)> _onToggle;
};

/// Spacer: empty; in a stack a Content-sized spacer stretches.
class SpacerImpl final : public TuiWidget<ui::SpacerWidget> {
public:
    explicit SpacerImpl(Context& context);
    [[nodiscard]] ::core::tui::Size naturalSize() const override { return {.width = 0, .height = 0}; }
    [[nodiscard]] bool expandsByDefault() const override { return true; }
};

}  // namespace morph::tui::detail
```

Create `src/tui/leaf_widgets.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/leaf_widgets.hpp"

#include <algorithm>
#include <core/tui/Theme.hpp>
#include <memory>
#include <utility>

namespace morph::tui::detail {

namespace {

::core::tui::Style const& roleStyle(::core::tui::Theme const& theme, ui::TextRole role) {
    if (role == ui::TextRole::Muted) {
        return theme.textMuted;
    }
    if (role == ui::TextRole::Heading) {
        return theme.textBold;
    }
    if (role == ui::TextRole::Error) {
        return theme.error;
    }
    if (role == ui::TextRole::Success) {
        return theme.success;
    }
    return theme.textNormal;
}

}  // namespace

TextImpl::TextImpl(Context& context) : TuiWidget{context} {
    adopt(std::make_unique<View>(*this));
}

void TextImpl::setText(std::string_view text) {
    _text = std::string{text};
    refresh();
}

void TextImpl::setRole(ui::TextRole role) {
    _role = role;
    refresh();
}

::core::tui::Size TextImpl::naturalSize() const {
    auto const lines = splitLines(_text);
    int width = 0;
    for (auto const line : lines) {
        width = std::max(width, displayWidth(line));
    }
    return {.width = width, .height = static_cast<int>(lines.size())};
}

void TextImpl::paint(::core::tui::Canvas& canvas) {
    auto const& style = roleStyle(canvas.theme(), _role);
    int row = 0;
    for (auto const line : splitLines(_text)) {
        if (row >= canvas.height()) {
            break;
        }
        canvas.putString(row, 0, line, style);
        ++row;
    }
}

ButtonImpl::ButtonImpl(Context& context) : TuiWidget{context} {
    adopt(std::make_unique<View>(*this));
}

void ButtonImpl::setLabel(std::string_view label) {
    _label = std::string{label};
    refresh();
}

void ButtonImpl::setOnClick(ui::Action onClick) {
    _onClick = std::move(onClick);
}

::core::tui::Size ButtonImpl::naturalSize() const {
    return {.width = displayWidth(_label) + 4, .height = 1};
}

void ButtonImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = !enabled() ? theme.buttonDisabled : hasFocus() ? theme.buttonFocused : theme.buttonNormal;
    canvas.putString(0, 0, "[ " + _label + " ]", style);
}

::core::tui::EventResult ButtonImpl::key(::core::tui::KeyEvent const& key) {
    if (!isActivation(key)) {
        return ::core::tui::EventResult::Ignored;
    }
    activate();
    return ::core::tui::EventResult::Handled;
}

void ButtonImpl::activate() {
    if (enabled() && _onClick) {
        _onClick();
    }
}

CheckboxImpl::CheckboxImpl(Context& context) : TuiWidget{context} {
    adopt(std::make_unique<View>(*this));
}

void CheckboxImpl::setLabel(std::string_view label) {
    _label = std::string{label};
    refresh();
}

void CheckboxImpl::setChecked(bool checked) {
    _checked = checked;
    refresh();
}

void CheckboxImpl::setOnToggle(std::function<void(bool)> onToggle) {
    _onToggle = std::move(onToggle);
}

::core::tui::Size CheckboxImpl::naturalSize() const {
    return {.width = displayWidth(_label) + 4, .height = 1};
}

void CheckboxImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = !enabled() ? theme.textMuted : hasFocus() ? theme.buttonFocused : theme.textNormal;
    canvas.putString(0, 0, std::string{_checked ? "[x] " : "[ ] "} + _label, style);
}

::core::tui::EventResult CheckboxImpl::key(::core::tui::KeyEvent const& key) {
    if (!isActivation(key)) {
        return ::core::tui::EventResult::Ignored;
    }
    activate();
    return ::core::tui::EventResult::Handled;
}

// Optimistic, like a text field: the box shows the new state at once, and a binding that
// disagrees sets it back through setChecked.
void CheckboxImpl::activate() {
    if (!enabled()) {
        return;
    }
    _checked = !_checked;
    refresh();
    if (_onToggle) {
        _onToggle(_checked);
    }
}

SpacerImpl::SpacerImpl(Context& context) : TuiWidget{context} {
    adopt(std::make_unique<View>(*this));
}

}  // namespace morph::tui::detail
```

Create `src/tui/container_widgets.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <morph/ui/backend.hpp>
#include <vector>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Column or Row (and ForEach, and a Table row): children along one axis, with a gap.
class StackImpl final : public TuiContainer<ui::StackWidget> {
public:
    StackImpl(Context& context, ui::Axis axis);
    void setGap(int gap) override;
    [[nodiscard]] ui::Axis axis() const noexcept { return _axis; }
    [[nodiscard]] int gap() const noexcept { return _gap; }
    /// Leading shown children given no area: what a Scroll around this stack has scrolled past.
    void setSkip(std::size_t skip);
    [[nodiscard]] std::size_t skip() const noexcept { return _skip; }
    /// Fixed main-axis extents and their gap, for a Table row; an empty list returns the row to the solver.
    void setColumnLayout(std::vector<int> extents, int gap);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    ui::Axis _axis;
    int _gap = 0;
    std::size_t _skip = 0;
    std::vector<int> _extents;
    int _extentGap = 0;
};

/// The slot a Switch case or a Tabs page mounts into: its children stacked vertically, no gap.
class SlotImpl final : public TuiContainer<ui::SlotWidget> {
public:
    explicit SlotImpl(Context& context);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;
};

}  // namespace morph::tui::detail
```

Create `src/tui/container_widgets.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/container_widgets.hpp"

#include <memory>
#include <numeric>
#include <utility>

namespace morph::tui::detail {

StackImpl::StackImpl(Context& context, ui::Axis axis) : TuiContainer{context}, _axis{axis} {
    adopt(std::make_unique<View>(*this));
}

void StackImpl::setGap(int gap) {
    _gap = gap;
    refresh();
}

void StackImpl::setSkip(std::size_t skip) {
    _skip = skip;
}

void StackImpl::setColumnLayout(std::vector<int> extents, int gap) {
    _extents = std::move(extents);
    _extentGap = gap;
}

::core::tui::Size StackImpl::naturalSize() const {
    if (_extents.empty()) {
        return stackNaturalSize(children(), _axis, _gap);
    }
    auto const natural = stackNaturalSize(children(), _axis, 0);
    int const columns = std::accumulate(_extents.begin(), _extents.end(), 0) +
                        (_extentGap * (static_cast<int>(_extents.size()) - 1));
    return _axis == ui::Axis::Horizontal ? ::core::tui::Size{.width = columns, .height = natural.height}
                                         : ::core::tui::Size{.width = natural.width, .height = columns};
}

void StackImpl::paint(::core::tui::Canvas& canvas) {
    arrangeStack(children(), canvas.area(),
                 StackSpec{.axis = _axis,
                           .gap = _extents.empty() ? _gap : _extentGap,
                           .skip = _skip,
                           .extents = _extents});
}

SlotImpl::SlotImpl(Context& context) : TuiContainer{context} {
    adopt(std::make_unique<View>(*this));
}

::core::tui::Size SlotImpl::naturalSize() const {
    return stackNaturalSize(children(), ui::Axis::Vertical, 0);
}

void SlotImpl::paint(::core::tui::Canvas& canvas) {
    arrangeStack(children(), canvas.area(), StackSpec{.axis = ui::Axis::Vertical});
}

}  // namespace morph::tui::detail
```

Add to `add_library(morph_tui STATIC …)`: `src/tui/context.cpp`, `src/tui/context.hpp`, `src/tui/widget.cpp`,
`src/tui/widget.hpp`, `src/tui/leaf_widgets.cpp`, `src/tui/leaf_widgets.hpp`, `src/tui/container_widgets.cpp`,
`src/tui/container_widgets.hpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[widgets]"`
Expected: PASS, 9 test cases.

Mutation check: in `WidgetBase::~WidgetBase`, delete the `setFocus(nullptr)` block. Expected: FAIL in "destroying
the focused widget clears the screen's focus" at `focusedComponent() == nullptr` (and, under ASan, a
use-after-free report from the `draw()` that follows, which reads the focused component's cursor shape). Restore.

- [ ] **Step 6: Commit**

```bash
git add src/tui tests/tui CMakeLists.txt
git commit -m "wip(tui): widget base, stacks, text, button, checkbox and spacer

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: Grid, Panel and Scroll

**Files:**
- Modify: `src/tui/container_widgets.hpp`, `src/tui/container_widgets.cpp` — append `GridImpl`, `PanelImpl`,
  `ScrollImpl`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_containers.cpp`
- Test: `tests/tui/test_tui_containers.cpp`

**Interfaces:**
- Consumes: `ui::GridWidget`, `ui::PanelWidget`, `ui::ScrollWidget` (Part 2); `layout::planGrid`, `slotArea`,
  `pad` (Task 7); `StackImpl::setSkip`, `arrangeStack`, `stackNaturalSize` (Task 8);
  `core::tui::BorderStyle` (core-cpp `tui/Box.hpp`).
- Produces: `GridImpl(Context&)`, `PanelImpl(Context&)`, `ScrollImpl(Context&, ui::Axis)`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_containers.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <memory>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::GridImpl;
using morph::tui::detail::PanelImpl;
using morph::tui::detail::ScrollImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

TEST_CASE("tui containers: a grid places cells row-major, a span covers columns and the gap", "[tui][containers]") {
    Harness harness{21, 3};
    auto const grid = harness.make<GridImpl>(nullptr);
    grid->setColumns(2);
    grid->setGap(1);
    auto const first = harness.make<TextImpl>(grid.get());
    first->setText("a");
    auto const second = harness.make<TextImpl>(grid.get());
    second->setText("b");
    auto const wide = harness.make<TextImpl>(grid.get());
    wide->setText("c");
    grid->setSpan(*wide, 2);
    CHECK(harness.draw() == Rows{"a          b", "", "c"});
}

TEST_CASE("tui containers: a panel draws its box and title around its child", "[tui][containers]") {
    Harness harness{12, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    CHECK(harness.draw() == Rows{"┌─Info─────┐", "│hi        │", "│          │", "└──────────┘"});
}

TEST_CASE("tui containers: a panel's padding insets its child", "[tui][containers]") {
    Harness harness{12, 5};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setPadding(1);
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    CHECK(harness.draw().at(2) == "│ hi       │");
}

TEST_CASE("tui containers: a collapsible panel toggles on Enter, hides its child and reports the state",
          "[tui][containers]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    panel->setCollapsed(false);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    CHECK(harness.draw().front() == "┌─▾ Info─────┐");
    harness.focus(*panel);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(toggles == std::vector<bool>{true});
    CHECK(harness.draw() == Rows{"▸ Info"});
}

TEST_CASE("tui containers: a scroll keeps the focused child in view", "[tui][containers]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    for (int i = 0; i < 5; ++i) {
        buttons.push_back(harness.make<ButtonImpl>(column.get()));
        buttons.back()->setLabel(std::to_string(i));
    }
    CHECK(harness.draw() == Rows{"[ 0 ]", "[ 1 ]", "[ 2 ]"});
    harness.focus(*buttons.back());
    CHECK(harness.draw() == Rows{"[ 2 ]", "[ 3 ]", "[ 4 ]"});
    harness.focus(*buttons.front());
    CHECK(harness.draw() == Rows{"[ 0 ]", "[ 1 ]", "[ 2 ]"});
    buttons.clear();
}

TEST_CASE("tui containers: the wheel scrolls a scroll with nothing focused in it", "[tui][containers]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<TextImpl>> lines;
    for (int i = 0; i < 5; ++i) {
        lines.push_back(harness.make<TextImpl>(column.get()));
        lines.back()->setText("line " + std::to_string(i));
    }
    static_cast<void>(harness.draw());
    static_cast<void>(harness.send(core::tui::MouseEvent{.type = core::tui::MouseEvent::Type::ScrollDown, .button = 0, .x = 1, .y = 1}));
    CHECK(harness.draw() == Rows{"line 1", "line 2", "line 3"});
    lines.clear();
}
```

Add `test_tui_containers.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `no type named 'GridImpl' in namespace 'morph::tui::detail'`.

- [ ] **Step 3: Implement**

Append to `src/tui/container_widgets.hpp`, inside the namespace (and add `#include <functional>`,
`#include <optional>`,
`#include <span>`, `#include <string>`, `#include <string_view>`, `#include <unordered_map>` and
`#include "tui/layout.hpp"`):

```cpp
/// Grid: equal columns, cells row-major, a cell spanning several columns.
class GridImpl final : public TuiContainer<ui::GridWidget> {
public:
    explicit GridImpl(Context& context);
    void setColumns(int columns) override;
    void setGap(int gap) override;
    void setSpan(ui::Widget& child, int span) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;

protected:
    void childForgotten(WidgetBase& child) override;

private:
    [[nodiscard]] int spanOf(WidgetBase const* child) const;
    [[nodiscard]] std::vector<layout::GridCell> cellsOf(std::span<WidgetBase* const> shown) const;

    int _columns = 1;
    int _gap = 0;
    std::unordered_map<WidgetBase const*, int> _spans;
};

/// Panel: a box with a title around its children, inset by padding; a collapsible panel folds to its title line.
class PanelImpl final : public TuiContainer<ui::PanelWidget> {
public:
    explicit PanelImpl(Context& context);
    void setTitle(std::string_view title) override;
    void setPadding(int padding) override;
    void setCollapsible(bool collapsible) override;
    void setCollapsed(bool collapsed) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return _collapsible; }
    [[nodiscard]] std::string probeText() const override { return _title; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;

protected:
    void childAttached(WidgetBase& child) override;

private:
    [[nodiscard]] std::string heading() const;
    void syncChildren();

    std::string _title;
    int _padding = 0;
    bool _collapsible = false;
    bool _collapsed = false;
    std::function<void(bool)> _onToggle;
};

/// Scroll: shows as much of its content as fits. When the content is a stack along the scroll's axis, whole
/// children scroll out of view, the focused one is always kept in view, and the wheel moves one child.
class ScrollImpl final : public TuiContainer<ui::ScrollWidget> {
public:
    ScrollImpl(Context& context, ui::Axis axis);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] bool wheel(int delta) override;

private:
    [[nodiscard]] StackImpl* scrolledStack() const;
    [[nodiscard]] std::size_t skipFor(StackImpl const& stack, int viewport) const;

    ui::Axis _axis;
    std::size_t _skip = 0;
};
```

Append to `src/tui/container_widgets.cpp`, inside the namespace (and add `#include <algorithm>`,
`#include <core/tui/Box.hpp>`, `#include <iterator>`, `#include <optional>` and `#include <string>`):

```cpp
namespace {

/// The extent of shown stack children [first, last] along the stack's axis, gaps included.
int extentOf(std::span<WidgetBase* const> items, std::size_t first, std::size_t last, StackImpl const& stack) {
    bool const vertical = stack.axis() == ui::Axis::Vertical;
    int extent = stack.gap() * static_cast<int>(last - first);
    for (auto const* item : items.subspan(first, last - first + 1)) {
        auto const natural = item->naturalSize();
        extent += vertical ? natural.height : natural.width;
    }
    return extent;
}

/// Which of @p items holds the screen's focus, itself or below it.
std::optional<std::size_t> indexHoldingFocus(std::span<WidgetBase* const> items, ::core::tui::Screen const& screen) {
    auto const* const focused = screen.focusedComponent();
    auto const found = std::ranges::find_if(items, [focused](WidgetBase const* item) { return isWithin(focused, item->view()); });
    if (found == items.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(items.begin(), found));
}

}  // namespace

GridImpl::GridImpl(Context& context) : TuiContainer{context} {
    adopt(std::make_unique<View>(*this));
}

void GridImpl::setColumns(int columns) {
    _columns = columns;
    refresh();
}

void GridImpl::setGap(int gap) {
    _gap = gap;
    refresh();
}

void GridImpl::setSpan(ui::Widget& child, int span) {
    _spans.insert_or_assign(&WidgetBase::of(child), span);
    refresh();
}

void GridImpl::childForgotten(WidgetBase& child) {
    _spans.erase(&child);
}

int GridImpl::spanOf(WidgetBase const* child) const {
    auto const found = _spans.find(child);
    return found == _spans.end() ? 1 : found->second;
}

std::vector<layout::GridCell> GridImpl::cellsOf(std::span<WidgetBase* const> shown) const {
    std::vector<layout::GridCell> cells;
    for (auto const* child : shown) {
        cells.push_back(layout::GridCell{
            .span = spanOf(child), .naturalHeight = child->naturalSize().height, .height = child->layout().height});
    }
    return cells;
}

::core::tui::Size GridImpl::naturalSize() const {
    auto const shown = shownChildren();
    if (shown.empty()) {
        return {.width = 0, .height = 0};
    }
    int const columns = std::max(1, _columns);
    int unit = 0;
    for (auto const* child : shown) {
        int const span = std::clamp(spanOf(child), 1, columns);
        unit = std::max(unit, (child->naturalSize().width + span - 1) / span);
    }
    int const width = (unit * columns) + (_gap * (columns - 1));
    auto const plan = layout::planGrid(_columns, cellsOf(shown), layout::Track{.length = width, .gap = _gap});
    int height = _gap * (static_cast<int>(plan.rowHeights.size()) - 1);
    for (int const rowHeight : plan.rowHeights) {
        height += rowHeight;
    }
    return {.width = width, .height = height};
}

void GridImpl::paint(::core::tui::Canvas& canvas) {
    for (auto* child : children()) {
        if (!child->shown()) {
            child->view().setArea({});
        }
    }
    auto const shown = shownChildren();
    auto const plan = layout::planGrid(_columns, cellsOf(shown), layout::Track{.length = canvas.width(), .gap = _gap});
    auto slot = plan.slots.begin();
    for (auto* child : shown) {
        child->view().setArea(layout::slotArea(plan, *slot));
        ++slot;
    }
}

PanelImpl::PanelImpl(Context& context) : TuiContainer{context} {
    adopt(std::make_unique<View>(*this));
}

void PanelImpl::setTitle(std::string_view title) {
    _title = std::string{title};
    refresh();
}

void PanelImpl::setPadding(int padding) {
    _padding = std::max(0, padding);
    refresh();
}

void PanelImpl::setCollapsible(bool collapsible) {
    _collapsible = collapsible;
    refresh();
}

void PanelImpl::setCollapsed(bool collapsed) {
    _collapsed = collapsed;
    syncChildren();
}

void PanelImpl::setOnToggle(std::function<void(bool)> onToggle) {
    _onToggle = std::move(onToggle);
}

std::string PanelImpl::heading() const {
    if (!_collapsible) {
        return _title;
    }
    return (_collapsed ? "▸ " : "▾ ") + _title;
}

void PanelImpl::childAttached(WidgetBase& child) {
    child.setStructuralVisible(!_collapsed);
}

void PanelImpl::syncChildren() {
    for (auto* child : children()) {
        child->setStructuralVisible(!_collapsed);
    }
    refresh();
}

::core::tui::Size PanelImpl::naturalSize() const {
    if (_collapsed) {
        return {.width = displayWidth(heading()), .height = 1};
    }
    auto const inner = stackNaturalSize(children(), ui::Axis::Vertical, 0);
    return {.width = std::max(inner.width + (2 * _padding) + 2, displayWidth(heading()) + 4),
            .height = inner.height + (2 * _padding) + 2};
}

void PanelImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = hasFocus() ? theme.buttonFocused : theme.textNormal;
    if (_collapsed) {
        canvas.putString(0, 0, heading(), style);
        return;
    }
    canvas.drawBox(canvas.area(), ::core::tui::BorderStyle::Single, style, heading());
    auto const inside = ::core::tui::Rect{.x = 1, .y = 1, .width = std::max(0, canvas.width() - 2),
                                          .height = std::max(0, canvas.height() - 2)};
    arrangeStack(children(), layout::pad(inside, _padding), StackSpec{.axis = ui::Axis::Vertical});
}

::core::tui::EventResult PanelImpl::key(::core::tui::KeyEvent const& key) {
    if (!_collapsible || !isActivation(key)) {
        return ::core::tui::EventResult::Ignored;
    }
    activate();
    return ::core::tui::EventResult::Handled;
}

void PanelImpl::activate() {
    if (!_collapsible || !enabled()) {
        return;
    }
    _collapsed = !_collapsed;
    syncChildren();
    if (_onToggle) {
        _onToggle(_collapsed);
    }
}

ScrollImpl::ScrollImpl(Context& context, ui::Axis axis) : TuiContainer{context}, _axis{axis} {
    adopt(std::make_unique<View>(*this));
}

::core::tui::Size ScrollImpl::naturalSize() const {
    return stackNaturalSize(children(), _axis, 0);
}

StackImpl* ScrollImpl::scrolledStack() const {
    auto const shown = shownChildren();
    if (shown.empty()) {
        return nullptr;
    }
    auto* const stack = dynamic_cast<StackImpl*>(shown.front());
    return stack != nullptr && stack->axis() == _axis ? stack : nullptr;
}

std::size_t ScrollImpl::skipFor(StackImpl const& stack, int viewport) const {
    auto const items = stack.shownChildren();
    if (items.empty()) {
        return 0;
    }
    std::size_t skip = std::min(_skip, items.size() - 1);
    if (auto const focused = indexHoldingFocus(items, *context().screen)) {
        if (*focused < skip) {
            skip = *focused;
        }
        while (skip < *focused && extentOf(items, skip, *focused, stack) > viewport) {
            ++skip;
        }
    }
    return skip;
}

void ScrollImpl::paint(::core::tui::Canvas& canvas) {
    arrangeStack(children(), canvas.area(), StackSpec{.axis = _axis});
    if (auto* const stack = scrolledStack()) {
        _skip = skipFor(*stack, _axis == ui::Axis::Vertical ? canvas.height() : canvas.width());
        stack->setSkip(_skip);
    }
}

bool ScrollImpl::wheel(int delta) {
    if (delta < 0) {
        _skip = _skip > 0 ? _skip - 1 : 0;
    } else {
        ++_skip;
    }
    refresh();
    return true;
}
```

`ScrollImpl::paint` writes `_skip`, so `paint` is not `const` — as declared.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[containers]"`
Expected: PASS, 6 test cases.

Mutation check: in `ScrollImpl::skipFor`, delete the `while (skip < *focused …)` loop. Expected: FAIL in "a scroll
keeps the focused child in view" (the second frame still shows `[ 0 ]`…`[ 2 ]`). Restore.

- [ ] **Step 5: Commit**

```bash
git add src/tui/container_widgets.hpp src/tui/container_widgets.cpp tests/tui
git commit -m "wip(tui): grid, panel and scroll

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 10: TextInput, DateTimeInput and FilePicker

**Files:**
- Create: `src/tui/field_widgets.hpp`, `src/tui/field_widgets.cpp`
- Modify: `CMakeLists.txt` — add both to `add_library(morph_tui STATIC …)`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_fields.cpp`
- Test: `tests/tui/test_tui_fields.cpp`

**Interfaces:**
- Consumes: `core::tui::InputField` (`processEvent`, `text`, `setText`, `setMasked`, `setMultiline`, `setPrompt`,
  `prompt`, `lineCount`, `render`) and `InputFieldAction` (core-cpp `tui/InputField.hpp`); `ui::TextInputWidget`,
  `ui::DateTimeInputWidget`, `ui::FilePickerWidget`, `ui::TextInputMode`, `ui::DateMode`, `ui::FilePickerMode`
  (Part 2); `morph::time::DateTime` (`toIso8601`, `fromIso8601`, `now`, `operator+`, `operator-`) and
  `morph::time::Timestamp` (`include/morph/util/datetime.hpp:30, 376`).
- Produces: `class FieldOwner`, `class FieldView`; `formatLocal(DateTime, ui::DateMode, int offsetMinutes)`,
  `parseLocal(std::string_view, ui::DateMode, int offsetMinutes)`; `TextInputImpl(Context&, ui::TextInputMode)`,
  `DateTimeInputImpl(Context&, ui::DateMode, int)`, `FilePickerImpl(Context&, ui::FilePickerMode)`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_fields.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <vector>

#include "tui/field_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
using core::tui::Modifier;
using morph::time::DateTime;
using morph::time::Timestamp;
using morph::tui::detail::DateTimeInputImpl;
using morph::tui::detail::FilePickerImpl;
using morph::tui::detail::formatLocal;
using morph::tui::detail::parseLocal;
using morph::tui::detail::TextInputImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

namespace {

DateTime at(int year, unsigned month, unsigned day, int hour, int minute) {
    return DateTime{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day},
                    std::chrono::hours{hour},  std::chrono::minutes{minute}, std::chrono::seconds{0}};
}

}  // namespace

TEST_CASE("tui fields: typing reports each change and Enter submits", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    std::vector<std::string> changes;
    std::vector<std::string> submits;
    input->setOnChange([&](std::string text) { changes.push_back(std::move(text)); });
    input->setOnSubmit([&](std::string text) { submits.push_back(std::move(text)); });
    harness.focus(*input);
    static_cast<void>(harness.type("hi"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(changes == std::vector<std::string>{"h", "hi"});
    CHECK(submits == std::vector<std::string>{"hi"});
    CHECK(harness.draw() == Rows{"hi"});
}

TEST_CASE("tui fields: setText with the field's own text keeps the cursor and fires nothing", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    std::vector<std::string> changes;
    input->setOnChange([&](std::string text) { changes.push_back(std::move(text)); });
    harness.focus(*input);
    static_cast<void>(harness.type("abc"));
    static_cast<void>(harness.key(KeyCode::Left));
    input->setText("abc");  // a binding echoing what was just typed
    CHECK(changes.size() == 3);
    static_cast<void>(harness.type("X"));
    CHECK(changes.back() == "abXc");
}

TEST_CASE("tui fields: setText with new text replaces it and fires nothing", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    int changes = 0;
    input->setOnChange([&](std::string const&) { ++changes; });
    input->setText("new");
    CHECK(changes == 0);
    CHECK(harness.draw() == Rows{"new"});
}

TEST_CASE("tui fields: Tab, Shift+Tab and Esc are left to the frontend and the dialog", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    input->setText("keep");
    harness.focus(*input);
    CHECK(harness.key(KeyCode::Tab) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Tab, Modifier::Shift) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Escape) == EventResult::Ignored);
    CHECK(harness.draw() == Rows{"keep"});
}

TEST_CASE("tui fields: the placeholder shows while the field is empty", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    input->setPlaceholder("name");
    CHECK(harness.draw() == Rows{"name"});
    harness.focus(*input);
    static_cast<void>(harness.type("x"));
    CHECK(harness.draw() == Rows{"x"});
}

TEST_CASE("tui fields: a password field never shows what was typed", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::Password);
    harness.focus(*input);
    static_cast<void>(harness.type("pw"));
    auto const rows = harness.draw();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().find("pw") == std::string::npos);
    CHECK_FALSE(rows.front().empty());
}

TEST_CASE("tui fields: local date-time text in a display zone", "[tui][fields]") {
    auto const instant = at(2026, 10, 4, 13, 5);
    CHECK(formatLocal(instant, ui::DateMode::DateTime, 0) == "2026-10-04 13:05");
    CHECK(formatLocal(instant, ui::DateMode::DateTime, 120) == "2026-10-04 15:05");
    CHECK(formatLocal(instant, ui::DateMode::Date, 0) == "2026-10-04");
    CHECK(parseLocal("2026-10-04 15:05", ui::DateMode::DateTime, 120) == instant);
    CHECK(parseLocal("2026-10-04T13:05", ui::DateMode::DateTime, 0) == instant);
    CHECK(parseLocal("2026-10-04", ui::DateMode::Date, 0) == at(2026, 10, 4, 0, 0));
    CHECK_FALSE(parseLocal("2026-13-01", ui::DateMode::Date, 0).has_value());
    CHECK_FALSE(parseLocal("garbage", ui::DateMode::DateTime, 0).has_value());
}

TEST_CASE("tui fields: a date-time field steps with the arrow keys and commits on Enter", "[tui][fields]") {
    Harness harness{20, 1};
    auto const field = harness.make<DateTimeInputImpl>(nullptr, ui::DateMode::DateTime, 0);
    std::vector<std::optional<Timestamp>> changes;
    field->setOnChange([&](std::optional<Timestamp> value) { changes.push_back(value); });
    field->setValue(Timestamp{at(2026, 10, 4, 13, 5)});
    CHECK(harness.draw() == Rows{"2026-10-04 13:05"});
    CHECK(changes.empty());

    harness.focus(*field);
    static_cast<void>(harness.key(KeyCode::Up));
    CHECK(harness.draw() == Rows{"2026-10-04 13:06"});
    REQUIRE(changes.size() == 1);
    CHECK(changes.back()->value == at(2026, 10, 4, 13, 6));

    static_cast<void>(harness.key(KeyCode::PageDown));
    CHECK(changes.back()->value == at(2026, 10, 3, 13, 6));

    field->setValue(std::nullopt);
    static_cast<void>(harness.type("2026-01-02 03:04"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(changes.back()->value == at(2026, 1, 2, 3, 4));

    field->setValue(std::nullopt);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK_FALSE(changes.back().has_value());
}

TEST_CASE("tui fields: an unparsable date-time is not committed and is marked", "[tui][fields]") {
    Harness harness{20, 1};
    auto const field = harness.make<DateTimeInputImpl>(nullptr, ui::DateMode::Date, 0);
    int changes = 0;
    field->setOnChange([&](std::optional<Timestamp> const&) { ++changes; });
    harness.focus(*field);
    static_cast<void>(harness.type("nope"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(changes == 0);
    CHECK(harness.draw().front().back() == '!');
}

TEST_CASE("tui fields: a file picker is a path field that reports on Enter", "[tui][fields]") {
    Harness harness{30, 1};
    auto const picker = harness.make<FilePickerImpl>(nullptr, ui::FilePickerMode::Open);
    std::vector<std::string> picked;
    picker->setOnPicked([&](std::string path) { picked.push_back(std::move(path)); });
    picker->setPath("/a");
    CHECK(harness.draw() == Rows{"Open: /a"});
    CHECK(picked.empty());
    harness.focus(*picker);
    static_cast<void>(harness.type("/b"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(picked == std::vector<std::string>{"/a/b"});
}
```

Add `test_tui_fields.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'tui/field_widgets.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/tui/field_widgets.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/tui/InputField.hpp>
#include <functional>
#include <morph/ui/backend.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// What a FieldView asks the widget that owns it.
class FieldOwner {
public:
    FieldOwner() = default;
    virtual ~FieldOwner() = default;
    FieldOwner(FieldOwner const&) = delete;
    FieldOwner& operator=(FieldOwner const&) = delete;
    FieldOwner(FieldOwner&&) = delete;
    FieldOwner& operator=(FieldOwner&&) = delete;

    /// The buffer or the cursor changed.
    virtual void edited() = 0;
    /// Enter was pressed.
    virtual void submitted() = 0;
    /// A key the owner handles before the field does; nullopt lets the field have it.
    [[nodiscard]] virtual std::optional<::core::tui::EventResult> intercept(::core::tui::KeyEvent const& key);
    /// Shown while the field is empty.
    [[nodiscard]] virtual std::string_view placeholder() const { return {}; }
    /// Whether to mark the field as holding text that does not parse.
    [[nodiscard]] virtual bool invalid() const { return false; }
};

/// A core::tui::InputField that leaves Tab, Shift+Tab and Esc to the frontend and to dialogs (InputField would
/// clear its buffer on Esc and cycle an agent mode on Shift+Tab), and reports edits and Enter to its owner.
class FieldView final : public Hosted<::core::tui::InputField> {
public:
    FieldView(WidgetBase& owner, FieldOwner& fields) : Hosted{owner}, _fields{&fields} {}
    [[nodiscard]] ::core::tui::EventResult onEvent(::core::tui::InputEvent const& event) override;
    void render(::core::tui::Canvas& canvas) override;

private:
    FieldOwner* _fields;
};

/// @p instant as text in the display zone @p offsetMinutes east of UTC: `YYYY-MM-DD` or `YYYY-MM-DD HH:MM`.
[[nodiscard]] std::string formatLocal(morph::time::DateTime instant, ui::DateMode mode, int offsetMinutes);
/// The instant @p text names in the display zone, or nullopt; DateTime mode accepts a space or a `T`.
[[nodiscard]] std::optional<morph::time::DateTime> parseLocal(std::string_view text, ui::DateMode mode, int offsetMinutes);

/// TextInput: single-line, multiline (Shift+Enter or Alt+Enter for a new line) or masked.
class TextInputImpl final : public TuiWidget<ui::TextInputWidget>, private FieldOwner {
public:
    TextInputImpl(Context& context, ui::TextInputMode mode);
    void setText(std::string_view text) override;
    void setPlaceholder(std::string_view placeholder) override;
    void setOnChange(std::function<void(std::string)> onChange) override;
    void setOnSubmit(std::function<void(std::string)> onSubmit) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;

private:
    void edited() override;
    void submitted() override;
    [[nodiscard]] std::string_view placeholder() const override { return _placeholder; }

    ui::TextInputMode _mode;
    FieldView* _field;
    std::string _reported;
    std::string _placeholder;
    std::function<void(std::string)> _onChange;
    std::function<void(std::string)> _onSubmit;
};

/// DateTimeInput: an ISO-style field in a display zone. Up/Down step by a day (Date) or a minute (DateTime),
/// PageUp/PageDown by a day; Enter commits what was typed, or clears the value when the field is empty.
class DateTimeInputImpl final : public TuiWidget<ui::DateTimeInputWidget>, private FieldOwner {
public:
    DateTimeInputImpl(Context& context, ui::DateMode mode, int offsetMinutes);
    void setValue(std::optional<morph::time::Timestamp> const& value) override;
    void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;

private:
    void edited() override;
    void submitted() override;
    [[nodiscard]] std::optional<::core::tui::EventResult> intercept(::core::tui::KeyEvent const& key) override;
    [[nodiscard]] bool invalid() const override { return _invalid; }
    void step(std::chrono::minutes delta);
    void commit(std::optional<morph::time::DateTime> value);

    ui::DateMode _mode;
    int _offsetMinutes;
    FieldView* _field;
    bool _invalid = false;
    std::function<void(std::optional<morph::time::Timestamp>)> _onChange;
};

/// FilePicker: a path field (a terminal has no file dialog), prompted `Open: ` or `Save: `; Enter reports it.
class FilePickerImpl final : public TuiWidget<ui::FilePickerWidget>, private FieldOwner {
public:
    FilePickerImpl(Context& context, ui::FilePickerMode mode);
    void setPath(std::string_view path) override;
    void setOnPicked(std::function<void(std::string)> onPicked) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;

private:
    void edited() override {}
    void submitted() override;

    FieldView* _field;
    std::function<void(std::string)> _onPicked;
};

}  // namespace morph::tui::detail
```

Create `src/tui/field_widgets.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/field_widgets.hpp"

#include <algorithm>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Theme.hpp>
#include <memory>
#include <utility>
#include <variant>

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

constexpr std::chrono::minutes kDay{24 * 60};

bool isPlain(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None;
}

}  // namespace

std::optional<EventResult> FieldOwner::intercept(::core::tui::KeyEvent const& /*key*/) {
    return std::nullopt;
}

EventResult FieldView::onEvent(::core::tui::InputEvent const& event) {
    if (auto const* mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
        return owner().pointer(*mouse);
    }
    if (!owner().enabled()) {
        return EventResult::Ignored;
    }
    if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event)) {
        if (key->key == KeyCode::Tab || key->key == KeyCode::Escape) {
            return EventResult::Ignored;
        }
        if (auto const handled = _fields->intercept(*key)) {
            return *handled;
        }
    }
    auto const action = processEvent(event);
    if (action == ::core::tui::InputFieldAction::Changed) {
        invalidate();
        _fields->edited();
        return EventResult::Handled;
    }
    if (action == ::core::tui::InputFieldAction::Submit) {
        _fields->submitted();
        return EventResult::Handled;
    }
    // None, and the agent-shell actions (Ctrl+T, Ctrl+G, …) that mean nothing here, bubble on.
    return EventResult::Ignored;
}

void FieldView::render(::core::tui::Canvas& canvas) {
    InputField::render(canvas);
    auto const& theme = canvas.theme();
    if (text().empty() && !_fields->placeholder().empty()) {
        canvas.putString(0, displayWidth(prompt()), _fields->placeholder(), theme.inputPlaceholder);
    }
    if (_fields->invalid() && canvas.width() > 0) {
        canvas.put(0, canvas.width() - 1, "!", theme.error);
    }
}

std::string formatLocal(morph::time::DateTime instant, ui::DateMode mode, int offsetMinutes) {
    // toIso8601: YYYY-MM-DDTHH:MM:SS.mmmZ
    auto const iso = (instant + std::chrono::minutes{offsetMinutes}).toIso8601();
    if (mode == ui::DateMode::Date) {
        return iso.substr(0, 10);
    }
    return iso.substr(0, 10) + " " + iso.substr(11, 5);
}

std::optional<morph::time::DateTime> parseLocal(std::string_view text, ui::DateMode mode, int offsetMinutes) {
    std::string iso;
    if (mode == ui::DateMode::Date) {
        if (text.size() != 10) {
            return std::nullopt;
        }
        iso = std::string{text} + "T00:00:00";
    } else {
        if (text.size() != 16 || (text.at(10) != ' ' && text.at(10) != 'T')) {
            return std::nullopt;
        }
        iso = std::string{text.substr(0, 10)} + "T" + std::string{text.substr(11, 5)} + ":00";
    }
    auto const local = morph::time::DateTime::fromIso8601(iso);
    if (!local) {
        return std::nullopt;
    }
    return *local - std::chrono::minutes{offsetMinutes};
}

TextInputImpl::TextInputImpl(Context& context, ui::TextInputMode mode)
    : TuiWidget{context}, _mode{mode}, _field{&adopt(std::make_unique<FieldView>(*this, static_cast<FieldOwner&>(*this)))} {
    _field->setMasked(mode == ui::TextInputMode::Password);
    _field->setMultiline(mode == ui::TextInputMode::Multiline);
}

// The guard is what keeps a controlled input usable: a binding that writes back the text the user just typed
// must not replace the buffer, which would move the cursor to the end.
void TextInputImpl::setText(std::string_view text) {
    if (text != _field->text()) {
        _field->setText(text);
        refresh();
    }
    _reported = std::string{text};
}

void TextInputImpl::setPlaceholder(std::string_view placeholder) {
    _placeholder = std::string{placeholder};
    refresh();
}

void TextInputImpl::setOnChange(std::function<void(std::string)> onChange) {
    _onChange = std::move(onChange);
}

void TextInputImpl::setOnSubmit(std::function<void(std::string)> onSubmit) {
    _onSubmit = std::move(onSubmit);
}

::core::tui::Size TextInputImpl::naturalSize() const {
    int const width = std::max({20, displayWidth(_field->text()) + 1, displayWidth(_placeholder)});
    int const height = _mode == ui::TextInputMode::Multiline ? std::max(3, _field->lineCount()) : 1;
    return {.width = width, .height = height};
}

std::string TextInputImpl::probeText() const {
    return std::string{_field->text()};
}

void TextInputImpl::edited() {
    std::string current{_field->text()};
    if (current == _reported) {
        return;
    }
    _reported = current;
    if (_onChange) {
        _onChange(std::move(current));
    }
}

void TextInputImpl::submitted() {
    if (_onSubmit) {
        _onSubmit(std::string{_field->text()});
    }
}

DateTimeInputImpl::DateTimeInputImpl(Context& context, ui::DateMode mode, int offsetMinutes)
    : TuiWidget{context},
      _mode{mode},
      _offsetMinutes{offsetMinutes},
      _field{&adopt(std::make_unique<FieldView>(*this, static_cast<FieldOwner&>(*this)))} {}

void DateTimeInputImpl::setValue(std::optional<morph::time::Timestamp> const& value) {
    auto const text = value && value->hasValue() ? formatLocal(**value, _mode, _offsetMinutes) : std::string{};
    if (text != _field->text()) {
        _field->setText(text);
    }
    _invalid = false;
    refresh();
}

void DateTimeInputImpl::setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) {
    _onChange = std::move(onChange);
}

::core::tui::Size DateTimeInputImpl::naturalSize() const {
    return {.width = _mode == ui::DateMode::Date ? 12 : 18, .height = 1};
}

std::string DateTimeInputImpl::probeText() const {
    return std::string{_field->text()};
}

void DateTimeInputImpl::edited() {
    _invalid = false;
}

void DateTimeInputImpl::submitted() {
    if (_field->text().empty()) {
        commit(std::nullopt);
        return;
    }
    if (auto const parsed = parseLocal(_field->text(), _mode, _offsetMinutes)) {
        commit(*parsed);
        return;
    }
    _invalid = true;
    refresh();
}

std::optional<EventResult> DateTimeInputImpl::intercept(::core::tui::KeyEvent const& key) {
    if (!isPlain(key)) {
        return std::nullopt;
    }
    auto const unit = _mode == ui::DateMode::Date ? kDay : std::chrono::minutes{1};
    if (key.key == KeyCode::Up) {
        step(unit);
    } else if (key.key == KeyCode::Down) {
        step(-unit);
    } else if (key.key == KeyCode::PageUp) {
        step(kDay);
    } else if (key.key == KeyCode::PageDown) {
        step(-kDay);
    } else {
        return std::nullopt;
    }
    return EventResult::Handled;
}

// An empty field steps from now; text that does not parse is left alone.
void DateTimeInputImpl::step(std::chrono::minutes delta) {
    auto const text = _field->text();
    auto const current = text.empty() ? std::optional{morph::time::DateTime::now()}
                                      : parseLocal(text, _mode, _offsetMinutes);
    if (!current) {
        _invalid = true;
        refresh();
        return;
    }
    commit(*current + delta);
}

void DateTimeInputImpl::commit(std::optional<morph::time::DateTime> value) {
    _invalid = false;
    _field->setText(value ? formatLocal(*value, _mode, _offsetMinutes) : std::string{});
    refresh();
    if (_onChange) {
        _onChange(value ? std::optional{morph::time::Timestamp{*value}} : std::nullopt);
    }
}

FilePickerImpl::FilePickerImpl(Context& context, ui::FilePickerMode mode)
    : TuiWidget{context}, _field{&adopt(std::make_unique<FieldView>(*this, static_cast<FieldOwner&>(*this)))} {
    _field->setPrompt(mode == ui::FilePickerMode::Open ? "Open: " : "Save: ");
}

void FilePickerImpl::setPath(std::string_view path) {
    if (path != _field->text()) {
        _field->setText(path);
        refresh();
    }
}

void FilePickerImpl::setOnPicked(std::function<void(std::string)> onPicked) {
    _onPicked = std::move(onPicked);
}

::core::tui::Size FilePickerImpl::naturalSize() const {
    return {.width = std::max(30, displayWidth(_field->prompt()) + displayWidth(_field->text()) + 1), .height = 1};
}

std::string FilePickerImpl::probeText() const {
    return std::string{_field->text()};
}

void FilePickerImpl::submitted() {
    if (_onPicked && !_field->text().empty()) {
        _onPicked(std::string{_field->text()});
    }
}

}  // namespace morph::tui::detail
```

Add `src/tui/field_widgets.cpp` and `src/tui/field_widgets.hpp` to `add_library(morph_tui STATIC …)`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[fields]"`
Expected: PASS, 10 test cases.

Mutation check: in `TextInputImpl::setText`, drop the `if (text != _field->text())` guard (always call
`_field->setText(text)`). Expected: FAIL in "setText with the field's own text keeps the cursor and fires nothing"
(`abcX` instead of `abXc`). Restore. Then in `FieldView::onEvent` delete the `KeyCode::Escape` test. Expected: FAIL
in "Tab, Shift+Tab and Esc are left to the frontend and the dialog" (InputField clears the buffer on Esc). Restore.

- [ ] **Step 5: Commit**

```bash
git add src/tui/field_widgets.hpp src/tui/field_widgets.cpp tests/tui CMakeLists.txt
git commit -m "wip(tui): text input, date-time input and file picker

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: Select, Menu and Tabs

**Files:**
- Create: `src/tui/list_widgets.hpp`, `src/tui/list_widgets.cpp`
- Modify: `CMakeLists.txt` — add both to `add_library(morph_tui STATIC …)`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_lists.cpp`
- Test: `tests/tui/test_tui_lists.cpp`

**Interfaces:**
- Consumes: `core::tui::List` (`setItems`, `selectedIndex`, `setSelectedIndex`, `items`, `onEvent`), `ListItem`
  (core-cpp `tui/List.hpp`); `Screen::showOverlay`, `hideOverlay`; `ui::SelectWidget`, `ui::MenuWidget`,
  `ui::TabsWidget`, `ui::SelectOption` (Part 2); `arrangeStack`, `isActivation` (Task 8).
- Produces: `class ListOwner`, `class ListView`; `RadioSelectImpl(Context&)`, `DropdownSelectImpl(Context&)` (with
  `isOpen()`), `MenuImpl(Context&)`, `TabsImpl(Context&)`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_lists.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/KeyCode.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/leaf_widgets.hpp"
#include "tui/list_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using morph::tui::detail::DropdownSelectImpl;
using morph::tui::detail::MenuImpl;
using morph::tui::detail::RadioSelectImpl;
using morph::tui::detail::TabsImpl;
using morph::tui::detail::TextImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

namespace {

ui::Key key(std::int64_t value) {
    return ui::Key{value};
}

std::vector<ui::SelectOption> colours() {
    return {{.key = key(1), .label = "Red"}, {.key = key(2), .label = "Green"}};
}

}  // namespace

TEST_CASE("tui lists: a radio select marks the selection and selects on Space", "[tui][lists]") {
    Harness harness{20, 2};
    auto const select = harness.make<RadioSelectImpl>(nullptr);
    std::vector<ui::Key> chosen;
    select->setOptions(colours());
    select->setSelected(key(2));
    select->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    CHECK(harness.draw() == Rows{"  ( ) Red", "▶ (•) Green"});
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Up));
    static_cast<void>(harness.type(" "));
    CHECK(chosen == std::vector<ui::Key>{key(1)});
    CHECK(harness.draw() == Rows{"▶ (•) Red", "  ( ) Green"});
}

TEST_CASE("tui lists: a dropdown opens its list as an overlay and closes on a choice", "[tui][lists]") {
    Harness harness{20, 4};
    auto const select = harness.make<DropdownSelectImpl>(nullptr);
    std::vector<ui::Key> chosen;
    select->setOptions(colours());
    select->setSelected(key(1));
    select->setOnSelect([&](ui::Key picked) { chosen.push_back(std::move(picked)); });
    CHECK(harness.draw() == Rows{"[Red ▾]"});
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(select->isOpen());
    CHECK(harness.draw() == Rows{"[Red ▾]", "▶ Red", "  Green"});
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK_FALSE(select->isOpen());
    CHECK(chosen == std::vector<ui::Key>{key(2)});
    CHECK(harness.focused(*select));
    CHECK(harness.draw() == Rows{"[Green ▾]"});
}

TEST_CASE("tui lists: Esc closes a dropdown without choosing", "[tui][lists]") {
    Harness harness{20, 4};
    auto const select = harness.make<DropdownSelectImpl>(nullptr);
    int chosen = 0;
    select->setOptions(colours());
    select->setOnSelect([&](ui::Key const&) { ++chosen; });
    harness.focus(*select);
    static_cast<void>(harness.key(KeyCode::Enter));
    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK_FALSE(select->isOpen());
    CHECK(chosen == 0);
    CHECK(harness.focused(*select));
}

TEST_CASE("tui lists: a menu is one list; Enter activates the highlighted item", "[tui][lists]") {
    Harness harness{20, 2};
    auto const menu = harness.make<MenuImpl>(nullptr);
    std::vector<std::size_t> activated;
    menu->setItems({"Open", "Quit"});
    menu->setOnActivate([&](std::size_t index) { activated.push_back(index); });
    CHECK(harness.draw() == Rows{"▶ Open", "  Quit"});
    harness.focus(*menu);
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(activated == std::vector<std::size_t>{1});
}

TEST_CASE("tui lists: tabs draw a bar over the pages the mount leaves visible; Right selects the next",
          "[tui][lists]") {
    Harness harness{20, 3};
    auto const tabs = harness.make<TabsImpl>(nullptr);
    std::vector<std::size_t> selected;
    tabs->setTabs({"One", "Two"});
    tabs->setSelected(0);
    tabs->setOnSelect([&](std::size_t index) { selected.push_back(index); });
    // As the mount does it: one page per tab shown so far, in first-shown order, every page but the selected one
    // hidden through its own visible flag.
    auto const first = harness.make<TextImpl>(tabs.get());
    first->setText("first");
    auto const second = harness.make<TextImpl>(tabs.get());
    second->setText("second");
    second->setVisible(false);
    CHECK(harness.draw() == Rows{"[One] Two", "first"});
    harness.focus(*tabs);
    static_cast<void>(harness.key(KeyCode::Right));
    CHECK(selected == std::vector<std::size_t>{1});
    // The bar moves at once; the pages stay as they are until the mount swaps them.
    CHECK(harness.draw() == Rows{" One [Two]", "first"});
    first->setVisible(false);
    second->setVisible(true);
    CHECK(harness.draw() == Rows{" One [Two]", "second"});
    static_cast<void>(harness.key(KeyCode::Right));
    CHECK(selected.size() == 1);
}

TEST_CASE("tui lists: a tab's page is not picked by its position among the children", "[tui][lists]") {
    Harness harness{20, 3};
    auto const tabs = harness.make<TabsImpl>(nullptr);
    tabs->setTabs({"One", "Two"});
    // Tab Two was shown first, so its page is the first child; tab One's page arrived second and is selected.
    auto const twoPage = harness.make<TextImpl>(tabs.get());
    twoPage->setText("two");
    twoPage->setVisible(false);
    auto const onePage = harness.make<TextImpl>(tabs.get());
    onePage->setText("one");
    tabs->setSelected(0);
    CHECK(harness.draw() == Rows{"[One] Two", "one"});
}
```

Add `test_tui_lists.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'tui/list_widgets.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/tui/list_widgets.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/List.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <optional>
#include <string>
#include <vector>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// What a ListView asks the widget that owns it.
class ListOwner {
public:
    ListOwner() = default;
    virtual ~ListOwner() = default;
    ListOwner(ListOwner const&) = delete;
    ListOwner& operator=(ListOwner const&) = delete;
    ListOwner(ListOwner&&) = delete;
    ListOwner& operator=(ListOwner&&) = delete;

    /// A key the owner handles before the list does; nullopt lets the list have it.
    [[nodiscard]] virtual std::optional<::core::tui::EventResult> listKey(::core::tui::KeyEvent const& key) = 0;
};

/// A core::tui::List that asks its owner first and leaves Esc to dialogs (List would report it as a cancel).
class ListView final : public Hosted<::core::tui::List> {
public:
    ListView(WidgetBase& owner, ListOwner& lists) : Hosted{owner}, _lists{&lists} {}
    [[nodiscard]] ::core::tui::EventResult onEvent(::core::tui::InputEvent const& event) override;

private:
    ListOwner* _lists;
};

/// Select, radio style: every option on its own row, `(•)` marking the selection; Space or Enter selects.
class RadioSelectImpl final : public TuiWidget<ui::SelectWidget>, private ListOwner {
public:
    explicit RadioSelectImpl(Context& context);
    void setOptions(std::vector<ui::SelectOption> const& options) override;
    void setSelected(std::optional<ui::Key> const& selected) override;
    void setOnSelect(std::function<void(ui::Key)> onSelect) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void activate() override;
    void click(::core::tui::Point cell) override;

private:
    [[nodiscard]] std::optional<::core::tui::EventResult> listKey(::core::tui::KeyEvent const& key) override;
    void rebuild();
    void choose(std::size_t index);

    ListView* _list;
    std::vector<ui::SelectOption> _options;
    std::optional<ui::Key> _selected;
    std::function<void(ui::Key)> _onSelect;
};

/// Select, dropdown style: one row `[label ▾]`; Enter, Space or Down opens the options as a list overlay below it.
class DropdownSelectImpl final : public TuiWidget<ui::SelectWidget> {
public:
    explicit DropdownSelectImpl(Context& context);
    ~DropdownSelectImpl() override;
    DropdownSelectImpl(DropdownSelectImpl const&) = delete;
    DropdownSelectImpl& operator=(DropdownSelectImpl const&) = delete;
    DropdownSelectImpl(DropdownSelectImpl&&) = delete;
    DropdownSelectImpl& operator=(DropdownSelectImpl&&) = delete;

    void setOptions(std::vector<ui::SelectOption> const& options) override;
    void setSelected(std::optional<ui::Key> const& selected) override;
    void setOnSelect(std::function<void(ui::Key)> onSelect) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    [[nodiscard]] bool isOpen() const noexcept { return _open; }

private:
    class Popup;
    void open();
    void close();
    void choose(std::size_t index);

    std::vector<ui::SelectOption> _options;
    std::optional<ui::Key> _selected;
    std::function<void(ui::Key)> _onSelect;
    std::unique_ptr<Popup> _popup;
    bool _open = false;
};

/// Menu: one list of item labels; Enter or Space activates the highlighted item.
class MenuImpl final : public TuiWidget<ui::MenuWidget>, private ListOwner {
public:
    explicit MenuImpl(Context& context);
    void setItems(std::vector<std::string> const& items) override;
    void setOnActivate(std::function<void(std::size_t)> onActivate) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void activate() override;
    void click(::core::tui::Point cell) override;

private:
    [[nodiscard]] std::optional<::core::tui::EventResult> listKey(::core::tui::KeyEvent const& key) override;

    ListView* _list;
    std::vector<std::string> _labels;
    std::function<void(std::size_t)> _onActivate;
};

/// Tabs: a bar (Left/Right, or a click, selects) above the body. The children are the page slots the mount makes,
/// in the order the tabs were first shown; they are stacked, and the mount hides every page but the selected one
/// through its own visible flag, so `setSelected` moves the bar's highlight only.
class TabsImpl final : public TuiContainer<ui::TabsWidget> {
public:
    explicit TabsImpl(Context& context);
    void setTabs(std::vector<std::string> const& tabs) override;
    void setSelected(std::size_t index) override;
    void setOnSelect(std::function<void(std::size_t)> onSelect) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void click(::core::tui::Point cell) override;

private:
    [[nodiscard]] std::string labelText(std::size_t index) const;
    void select(std::size_t index);

    std::vector<std::string> _labels;
    std::size_t _selected = 0;
    std::function<void(std::size_t)> _onSelect;
};

}  // namespace morph::tui::detail
```

Create `src/tui/list_widgets.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/list_widgets.hpp"

#include <algorithm>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Theme.hpp>
#include <iterator>
#include <utility>
#include <variant>

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

std::optional<std::size_t> indexOf(std::vector<ui::SelectOption> const& options, std::optional<ui::Key> const& key) {
    if (!key) {
        return std::nullopt;
    }
    auto const found = std::ranges::find_if(options, [&key](ui::SelectOption const& option) { return option.key == *key; });
    if (found == options.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(options.begin(), found));
}

std::string labelOf(std::vector<ui::SelectOption> const& options, std::optional<ui::Key> const& key) {
    auto const index = indexOf(options, key);
    return index ? options.at(*index).label : std::string{};
}

int widestLabel(std::vector<ui::SelectOption> const& options) {
    int widest = 0;
    for (auto const& option : options) {
        widest = std::max(widest, displayWidth(option.label));
    }
    return widest;
}

bool isPlain(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None;
}

}  // namespace

EventResult ListView::onEvent(::core::tui::InputEvent const& event) {
    if (auto const* mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
        return owner().pointer(*mouse);
    }
    if (!owner().enabled()) {
        return EventResult::Ignored;
    }
    if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event)) {
        if (auto const handled = _lists->listKey(*key)) {
            return *handled;
        }
        if (key->key == KeyCode::Escape) {
            return EventResult::Ignored;
        }
    }
    return List::onEvent(event);
}

RadioSelectImpl::RadioSelectImpl(Context& context)
    : TuiWidget{context}, _list{&adopt(std::make_unique<ListView>(*this, static_cast<ListOwner&>(*this)))} {}

void RadioSelectImpl::setOptions(std::vector<ui::SelectOption> const& options) {
    _options = options;
    rebuild();
}

void RadioSelectImpl::setSelected(std::optional<ui::Key> const& selected) {
    _selected = selected;
    rebuild();
    if (auto const index = indexOf(_options, _selected)) {
        _list->setSelectedIndex(*index);
    }
}

void RadioSelectImpl::setOnSelect(std::function<void(ui::Key)> onSelect) {
    _onSelect = std::move(onSelect);
}

::core::tui::Size RadioSelectImpl::naturalSize() const {
    // "▶ " or "  ", then "(•) " or "( ) ", then the label.
    return {.width = widestLabel(_options) + 6, .height = std::max(1, static_cast<int>(_options.size()))};
}

std::string RadioSelectImpl::probeText() const {
    return labelOf(_options, _selected);
}

void RadioSelectImpl::activate() {
    choose(_list->selectedIndex());
}

void RadioSelectImpl::click(::core::tui::Point cell) {
    if (cell.y >= 0 && static_cast<std::size_t>(cell.y) < _options.size()) {
        _list->setSelectedIndex(static_cast<std::size_t>(cell.y));
        activate();
    }
}

std::optional<EventResult> RadioSelectImpl::listKey(::core::tui::KeyEvent const& key) {
    if (!isActivation(key)) {
        return std::nullopt;
    }
    activate();
    return EventResult::Handled;
}

// core::tui::List::setItems puts the highlight back on the first row; the row the user was on is kept.
void RadioSelectImpl::rebuild() {
    std::vector<::core::tui::ListItem> items;
    for (auto const& option : _options) {
        items.push_back(::core::tui::ListItem{.label = (_selected == option.key ? "(•) " : "( ) ") + option.label});
    }
    auto const highlight = _list->selectedIndex();
    _list->setItems(std::move(items));
    if (!_options.empty()) {
        _list->setSelectedIndex(std::min(highlight, _options.size() - 1));
    }
    refresh();
}

void RadioSelectImpl::choose(std::size_t index) {
    if (index >= _options.size()) {
        return;
    }
    auto const picked = _options.at(index).key;
    _selected = picked;
    rebuild();
    if (_onSelect) {
        _onSelect(picked);
    }
}

/// The dropdown's option list, shown as an overlay. It has no parent, so keys stop here.
class DropdownSelectImpl::Popup final : public ::core::tui::List {
public:
    explicit Popup(DropdownSelectImpl& owner) : _owner{&owner} {}

    [[nodiscard]] EventResult onEvent(::core::tui::InputEvent const& event) override {
        if (std::holds_alternative<::core::tui::MouseEvent>(event)) {
            return EventResult::Handled;
        }
        if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event)) {
            if (isActivation(*key)) {
                _owner->choose(selectedIndex());
                return EventResult::Handled;
            }
            if (key->key == KeyCode::Escape) {
                _owner->close();
                return EventResult::Handled;
            }
            if (key->key == KeyCode::Tab) {
                _owner->close();
                return EventResult::Ignored;
            }
        }
        return List::onEvent(event);
    }

private:
    DropdownSelectImpl* _owner;
};

DropdownSelectImpl::DropdownSelectImpl(Context& context) : TuiWidget{context}, _popup{std::make_unique<Popup>(*this)} {
    adopt(std::make_unique<View>(*this));
}

// NOLINTNEXTLINE(bugprone-exception-escape): closing hands focus back and hides the overlay; neither may be skipped.
DropdownSelectImpl::~DropdownSelectImpl() {
    close();
}

void DropdownSelectImpl::setOptions(std::vector<ui::SelectOption> const& options) {
    _options = options;
    refresh();
}

void DropdownSelectImpl::setSelected(std::optional<ui::Key> const& selected) {
    _selected = selected;
    refresh();
}

void DropdownSelectImpl::setOnSelect(std::function<void(ui::Key)> onSelect) {
    _onSelect = std::move(onSelect);
}

::core::tui::Size DropdownSelectImpl::naturalSize() const {
    return {.width = widestLabel(_options) + 4, .height = 1};
}

std::string DropdownSelectImpl::probeText() const {
    return labelOf(_options, _selected);
}

void DropdownSelectImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const& style = !enabled() ? theme.buttonDisabled : hasFocus() ? theme.buttonFocused : theme.buttonNormal;
    canvas.putString(0, 0, "[" + labelOf(_options, _selected) + " ▾]", style);
}

EventResult DropdownSelectImpl::key(::core::tui::KeyEvent const& key) {
    if (isActivation(key) || (isPlain(key) && key.key == KeyCode::Down)) {
        open();
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void DropdownSelectImpl::activate() {
    open();
}

void DropdownSelectImpl::open() {
    if (_open || _options.empty() || !enabled()) {
        return;
    }
    std::vector<::core::tui::ListItem> items;
    for (auto const& option : _options) {
        items.push_back(::core::tui::ListItem{.label = option.label});
    }
    _popup->setItems(std::move(items));
    if (auto const index = indexOf(_options, _selected)) {
        _popup->setSelectedIndex(*index);
    }
    auto& screen = *context().screen;
    auto const bounds = view().screenBounds();
    screen.showOverlay(*_popup, ::core::tui::Point{.x = bounds.x, .y = bounds.y + 1});
    screen.setFocus(_popup.get());
    _open = true;
    refresh();
}

void DropdownSelectImpl::close() {
    if (!_open) {
        return;
    }
    auto& screen = *context().screen;
    bool const hadFocus = screen.focusedComponent() == _popup.get();
    screen.hideOverlay(*_popup);
    _open = false;
    if (hadFocus) {
        screen.setFocus(&view());
    }
    refresh();
}

// Optimistic, like the other choosers: the field shows the choice at once.
void DropdownSelectImpl::choose(std::size_t index) {
    if (index >= _options.size()) {
        close();
        return;
    }
    auto const picked = _options.at(index).key;
    _selected = picked;
    close();
    if (_onSelect) {
        _onSelect(picked);
    }
}

MenuImpl::MenuImpl(Context& context) : TuiWidget{context}, _list{&adopt(std::make_unique<ListView>(*this, static_cast<ListOwner&>(*this)))} {}

void MenuImpl::setItems(std::vector<std::string> const& items) {
    _labels = items;
    std::vector<::core::tui::ListItem> rows;
    for (auto const& label : _labels) {
        rows.push_back(::core::tui::ListItem{.label = label});
    }
    auto const highlight = _list->selectedIndex();
    _list->setItems(std::move(rows));
    if (!_labels.empty()) {
        _list->setSelectedIndex(std::min(highlight, _labels.size() - 1));
    }
    refresh();
}

void MenuImpl::setOnActivate(std::function<void(std::size_t)> onActivate) {
    _onActivate = std::move(onActivate);
}

::core::tui::Size MenuImpl::naturalSize() const {
    int widest = 0;
    for (auto const& label : _labels) {
        widest = std::max(widest, displayWidth(label));
    }
    return {.width = widest + 2, .height = std::max(1, static_cast<int>(_labels.size()))};
}

std::string MenuImpl::probeText() const {
    auto const index = _list->selectedIndex();
    return index < _labels.size() ? _labels.at(index) : std::string{};
}

void MenuImpl::activate() {
    if (_onActivate && !_labels.empty()) {
        _onActivate(_list->selectedIndex());
    }
}

void MenuImpl::click(::core::tui::Point cell) {
    if (cell.y >= 0 && static_cast<std::size_t>(cell.y) < _labels.size()) {
        _list->setSelectedIndex(static_cast<std::size_t>(cell.y));
        activate();
    }
}

std::optional<EventResult> MenuImpl::listKey(::core::tui::KeyEvent const& key) {
    if (!isActivation(key)) {
        return std::nullopt;
    }
    activate();
    return EventResult::Handled;
}

TabsImpl::TabsImpl(Context& context) : TuiContainer{context} {
    adopt(std::make_unique<View>(*this));
}

void TabsImpl::setTabs(std::vector<std::string> const& tabs) {
    _labels = tabs;
    refresh();
}

// The highlight only: which page shows is the mount's, through each page slot's own visible flag. A child's
// position says nothing about its tab, because the pages arrive in the order their tabs were first shown.
void TabsImpl::setSelected(std::size_t index) {
    _selected = index;
    refresh();
}

void TabsImpl::setOnSelect(std::function<void(std::size_t)> onSelect) {
    _onSelect = std::move(onSelect);
}

std::string TabsImpl::labelText(std::size_t index) const {
    auto const& label = _labels.at(index);
    return index == _selected ? "[" + label + "]" : " " + label + " ";
}

::core::tui::Size TabsImpl::naturalSize() const {
    int bar = 0;
    for (std::size_t index = 0; index < _labels.size(); ++index) {
        bar += displayWidth(labelText(index));
    }
    auto const body = stackNaturalSize(children(), ui::Axis::Vertical, 0);
    return {.width = std::max(bar, body.width), .height = 1 + body.height};
}

std::string TabsImpl::probeText() const {
    return _selected < _labels.size() ? _labels.at(_selected) : std::string{};
}

void TabsImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    int column = 0;
    for (std::size_t index = 0; index < _labels.size(); ++index) {
        auto const& style = index != _selected ? theme.textMuted : hasFocus() ? theme.buttonFocused : theme.textBold;
        column += canvas.putString(0, column, labelText(index), style);
    }
    arrangeStack(children(),
                 ::core::tui::Rect{.x = 0, .y = 1, .width = canvas.width(), .height = std::max(0, canvas.height() - 1)},
                 StackSpec{.axis = ui::Axis::Vertical});
}

EventResult TabsImpl::key(::core::tui::KeyEvent const& key) {
    if (!isPlain(key)) {
        return EventResult::Ignored;
    }
    if (key.key == KeyCode::Left) {
        if (_selected > 0) {
            select(_selected - 1);
        }
        return EventResult::Handled;
    }
    if (key.key == KeyCode::Right) {
        if (_selected + 1 < _labels.size()) {
            select(_selected + 1);
        }
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

void TabsImpl::click(::core::tui::Point cell) {
    if (cell.y != 0) {
        return;
    }
    int right = 0;
    for (std::size_t index = 0; index < _labels.size(); ++index) {
        right += displayWidth(labelText(index));
        if (cell.x < right) {
            select(index);
            return;
        }
    }
}

// Optimistic, like the other choosers: the bar shows the choice at once, and the mount swaps the pages.
void TabsImpl::select(std::size_t index) {
    if (index >= _labels.size() || index == _selected) {
        return;
    }
    _selected = index;
    refresh();
    if (_onSelect) {
        _onSelect(index);
    }
}

}  // namespace morph::tui::detail
```

Add `src/tui/list_widgets.cpp` and `src/tui/list_widgets.hpp` to `add_library(morph_tui STATIC …)`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[lists]"`
Expected: PASS, 6 test cases.

Mutation checks, one at a time, each restored before the next:
- In `ListView::onEvent`, delete the `KeyCode::Escape` test, so Esc reaches `List`; then in
  `DropdownSelectImpl::Popup::onEvent` delete its `Escape` branch. Expected: FAIL in "Esc closes a dropdown without
  choosing" (`isOpen()` stays true).
- In `TabsImpl::setSelected`, hide every child but the one at position `index` (`child->setStructuralVisible(position
  == index)` over `children()`). Expected: FAIL in "a tab's page is not picked by its position among the children"
  (the second row is empty: the page at position 0 is tab Two's, which the mount hid).

- [ ] **Step 5: Commit**

```bash
git add src/tui/list_widgets.hpp src/tui/list_widgets.cpp tests/tui CMakeLists.txt
git commit -m "wip(tui): radio and dropdown select, menu and tabs

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 12: Dialog with its focus trap, Busy and Slider

**Files:**
- Modify: `src/tui/container_widgets.hpp`, `src/tui/container_widgets.cpp` — append `DialogImpl`
- Modify: `src/tui/leaf_widgets.hpp`, `src/tui/leaf_widgets.cpp` — append `BusyImpl`, `SliderImpl`
- Modify: `src/tui/widget.hpp`, `src/tui/widget.cpp` — `centredIn`, the dialog-aware `fit` and `moveFocus`, and
  `WidgetBase::pointer`'s dialog check
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_dialog.cpp`
- Test: `tests/tui/test_tui_dialog.cpp`

**Interfaces:**
- Consumes: `Screen::showOverlay`, `hideOverlay`, `positionOverlay`, `viewportArea`, `setFocus`,
  `focusedComponent`; `ui::DialogWidget`, `ui::BusyWidget`, `ui::SliderWidget` (Part 2); `Context::openDialogs`,
  `activeBusy`, `animationFrame` (Task 8).
- Produces: `DialogImpl(Context&)` (with `isOpen()`); `BusyImpl(Context&)`; `SliderImpl(Context&)`;
  `centredIn(Screen const&, Size) -> Point`; `fit` now centres open dialogs; `moveFocus` now cycles inside the
  innermost open dialog; a press outside the innermost open dialog is swallowed.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_dialog.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/KeyCode.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using morph::tui::detail::BusyImpl;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::DialogImpl;
using morph::tui::detail::Direction;
using morph::tui::detail::moveFocus;
using morph::tui::detail::SliderImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

namespace {

/// A column with a "Behind" button, and a dialog titled "Confirm" holding the given buttons.
struct Scene {
    explicit Scene(Harness& harness, std::vector<std::string> const& labels)
        : column{harness.make<StackImpl>(nullptr, ui::Axis::Vertical)},
          behind{harness.make<ButtonImpl>(column.get())},
          dialog{harness.make<DialogImpl>(column.get())},
          body{harness.make<StackImpl>(dialog.get(), ui::Axis::Vertical)} {
        behind->setLabel("Behind");
        dialog->setTitle("Confirm");
        for (auto const& label : labels) {
            buttons.push_back(harness.make<ButtonImpl>(body.get()));
            buttons.back()->setLabel(label);
        }
    }
    ~Scene() { buttons.clear(); }
    Scene(Scene const&) = delete;
    Scene& operator=(Scene const&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    std::unique_ptr<StackImpl> column;
    std::unique_ptr<ButtonImpl> behind;
    std::unique_ptr<DialogImpl> dialog;
    std::unique_ptr<StackImpl> body;
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
};

}  // namespace

TEST_CASE("tui dialog: an open dialog is a centred box over the tree, and takes the focus", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {}};
    auto const text = harness.make<TextImpl>(scene.body.get());
    text->setText("Sure?");
    scene.buttons.push_back(harness.make<ButtonImpl>(scene.body.get()));
    scene.buttons.back()->setLabel("OK");
    scene.dialog->setOpen(true);
    CHECK(harness.draw() == Rows{"[ Behind ]", "", "", "         ┌─Confirm─┐", "         │ Sure?   │",
                                 "         │ [ OK ]  │", "         └─────────┘"});
    CHECK(harness.focused(*scene.buttons.back()));
    scene.buttons.clear();
}

TEST_CASE("tui dialog: Esc anywhere inside calls onDismiss", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int dismissed = 0;
    scene.dialog->setOnDismiss([&] { ++dismissed; });
    scene.dialog->setOpen(true);
    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK(dismissed == 1);
}

TEST_CASE("tui dialog: Tab stays inside the open dialog", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK", "Cancel"}};
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    CHECK(harness.focused(*scene.buttons.at(0)));
    moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*scene.buttons.at(1)));
    moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*scene.buttons.at(0)));
    moveFocus(harness.context(), Direction::Backward);
    CHECK(harness.focused(*scene.buttons.at(1)));
}

TEST_CASE("tui dialog: closing hands the focus back to where it was", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    harness.focus(*scene.behind);
    scene.dialog->setOpen(true);
    CHECK(harness.focused(*scene.buttons.at(0)));
    scene.dialog->setOpen(false);
    CHECK(harness.focused(*scene.behind));
    CHECK(harness.draw() == Rows{"[ Behind ]"});
}

// The mount's order: a Dialog's content is destroyed before `setOpen(false)`, and destroying the focused button
// clears the screen's focus on the way.
TEST_CASE("tui dialog: closing after its content is gone still hands the focus back", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    harness.focus(*scene.behind);
    scene.dialog->setOpen(true);
    REQUIRE(harness.focused(*scene.buttons.at(0)));
    scene.buttons.clear();
    REQUIRE(harness.screen().focusedComponent() == nullptr);
    scene.dialog->setOpen(false);
    CHECK(harness.focused(*scene.behind));
}

TEST_CASE("tui dialog: a click behind an open dialog does nothing", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int behindClicks = 0;
    scene.behind->setOnClick([&] { ++behindClicks; });
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    static_cast<void>(harness.click({.x = 2, .y = 0}));
    CHECK(behindClicks == 0);
}

TEST_CASE("tui busy: spins while active, one frame per animation step", "[tui][busy]") {
    Harness harness{20, 1};
    auto const busy = harness.make<BusyImpl>(nullptr);
    busy->setLabel("Loading");
    CHECK(harness.draw().empty());
    busy->setActive(true);
    CHECK(harness.context().activeBusy == 1);
    CHECK(harness.draw() == Rows{"| Loading"});
    ++harness.context().animationFrame;
    CHECK(harness.draw() == Rows{"/ Loading"});
    busy->setActive(false);
    CHECK(harness.context().activeBusy == 0);
}

TEST_CASE("tui slider: Left and Right move by step, Home and End to the ends", "[tui][slider]") {
    Harness harness{24, 1};
    auto const slider = harness.make<SliderImpl>(nullptr);
    std::vector<std::int64_t> changes;
    slider->setRange(0, 100, 10);
    slider->setValue(50);
    slider->setOnChange([&](std::int64_t value) { changes.push_back(value); });
    CHECK(harness.draw() == Rows{"[=========|---------] 50"});
    harness.focus(*slider);
    static_cast<void>(harness.key(KeyCode::Right));
    CHECK(changes == std::vector<std::int64_t>{60});
    CHECK(harness.draw() == Rows{"[==========|--------] 60"});
    static_cast<void>(harness.key(KeyCode::Home));
    CHECK(harness.draw() == Rows{"[|-------------------] 0"});
    static_cast<void>(harness.key(KeyCode::Left));
    CHECK(changes == std::vector<std::int64_t>{60, 0});
}
```

Add `test_tui_dialog.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `no type named 'DialogImpl' in namespace 'morph::tui::detail'`.

- [ ] **Step 3: Make focus and fitting dialog-aware**

In `src/tui/widget.hpp`, add after `isWithin`:

```cpp
/// The top-left corner that centres an overlay of @p size on @p screen's viewport.
[[nodiscard]] ::core::tui::Point centredIn(::core::tui::Screen const& screen, ::core::tui::Size size);
```

and change the `fit` and `moveFocus` comments to "Sizes every root widget to the viewport and centres every open
dialog." and "Moves keyboard focus to the next or previous focusable component — inside the innermost open dialog
when one is open, because `Screen`'s own walk does not know overlays." Add
`[[nodiscard]] bool blockedByDialog() const;`
to `WidgetBase`'s private section.

In `src/tui/widget.cpp`, replace `fit` and `moveFocus` with:

```cpp
::core::tui::Point centredIn(::core::tui::Screen const& screen, ::core::tui::Size size) {
    auto const area = screen.viewportArea();
    return {.x = std::max(0, (area.width - size.width) / 2), .y = std::max(0, (area.height - size.height) / 2)};
}

void fit(Context& context) {
    auto& screen = *context.screen;
    auto const area = screen.viewportArea();
    for (auto* root : context.roots) {
        root->view().setArea(area);
    }
    for (auto* frame : context.openDialogs) {
        screen.positionOverlay(*frame, centredIn(screen, frame->preferredSize()));
    }
}

void moveFocus(Context& context, Direction direction) {
    auto& screen = *context.screen;
    if (context.openDialogs.empty()) {
        if (direction == Direction::Forward) {
            screen.focusNext();
        } else {
            screen.focusPrev();
        }
        return;
    }
    auto& frame = *context.openDialogs.back();
    std::vector<::core::tui::Component*> candidates;
    collectFocusable(frame, candidates);
    if (candidates.empty()) {
        screen.setFocus(&frame);
        return;
    }
    auto const count = static_cast<std::ptrdiff_t>(candidates.size());
    auto const current = std::ranges::find(candidates, screen.focusedComponent());
    std::ptrdiff_t index = direction == Direction::Forward ? 0 : count - 1;
    if (current != candidates.end()) {
        auto const position = std::distance(candidates.begin(), current);
        index = direction == Direction::Forward ? (position + 1) % count : (position + count - 1) % count;
    }
    screen.setFocus(*std::next(candidates.begin(), index));
}
```

Add, after `WidgetBase::refresh`:

```cpp
bool WidgetBase::blockedByDialog() const {
    return !_context->openDialogs.empty() && !isWithin(_view.get(), *_context->openDialogs.back());
}
```

and make it the first statement of `WidgetBase::pointer`:

```cpp
    if (blockedByDialog()) {
        return EventResult::Handled;
    }
```

- [ ] **Step 4: Implement Dialog, Busy and Slider**

Append to `src/tui/container_widgets.hpp` (inside the namespace; add `#include <memory>` and
`#include <morph/ui/view.hpp>`):

```cpp
/// Dialog: an overlay box centred on the screen, holding the dialog's children. Opening it moves the focus inside
/// and Tab then cycles there; Esc calls onDismiss; closing hands the focus back. Its place in the tree is an empty
/// anchor.
class DialogImpl final : public TuiContainer<ui::DialogWidget> {
public:
    explicit DialogImpl(Context& context);
    ~DialogImpl() override;
    DialogImpl(DialogImpl const&) = delete;
    DialogImpl& operator=(DialogImpl const&) = delete;
    DialogImpl(DialogImpl&&) = delete;
    DialogImpl& operator=(DialogImpl&&) = delete;

    void setOpen(bool open) override;
    void setTitle(std::string_view title) override;
    void setOnDismiss(ui::Action onDismiss) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override { return {.width = 0, .height = 0}; }
    [[nodiscard]] std::string probeText() const override { return _title; }
    [[nodiscard]] ::core::tui::Component& host() override;
    [[nodiscard]] bool isOpen() const noexcept { return _open; }

private:
    class Frame;
    [[nodiscard]] ::core::tui::Size frameSize() const;
    void paintFrame(::core::tui::Canvas& canvas);
    [[nodiscard]] ::core::tui::EventResult frameEvent(::core::tui::InputEvent const& event);
    void open();
    void close();

    std::unique_ptr<Frame> _frame;
    std::string _title;
    ui::Action _onDismiss;
    bool _open = false;
    ::core::tui::Component* _restoreFocus = nullptr;
};
```

Append to `src/tui/container_widgets.cpp` (inside the namespace; add `#include <core/tui/KeyCode.hpp>` and
`#include <variant>`):

```cpp
/// The dialog's overlay: it paints the box and owns the children's views. Focusable, so Esc reaches it even when
/// the dialog holds nothing focusable.
class DialogImpl::Frame final : public ::core::tui::Component {
public:
    explicit Frame(DialogImpl& owner) : _owner{&owner} {}
    void render(::core::tui::Canvas& canvas) override { _owner->paintFrame(canvas); }
    [[nodiscard]] ::core::tui::Size preferredSize() const override { return _owner->frameSize(); }
    [[nodiscard]] ::core::tui::EventResult onEvent(::core::tui::InputEvent const& event) override {
        return _owner->frameEvent(event);
    }
    [[nodiscard]] bool focusable() const override { return true; }

private:
    DialogImpl* _owner;
};

DialogImpl::DialogImpl(Context& context) : TuiContainer{context}, _frame{std::make_unique<Frame>(*this)} {
    adopt(std::make_unique<View>(*this));
}

// NOLINTNEXTLINE(bugprone-exception-escape): closing hides the overlay and hands focus back; neither may be skipped.
DialogImpl::~DialogImpl() {
    close();
}

void DialogImpl::setOpen(bool open) {
    if (open) {
        this->open();
    } else {
        close();
    }
}

void DialogImpl::setTitle(std::string_view title) {
    _title = std::string{title};
    refresh();
}

void DialogImpl::setOnDismiss(ui::Action onDismiss) {
    _onDismiss = std::move(onDismiss);
}

::core::tui::Component& DialogImpl::host() {
    return *_frame;
}

::core::tui::Size DialogImpl::frameSize() const {
    auto const inner = stackNaturalSize(children(), ui::Axis::Vertical, 0);
    auto const area = context().screen->viewportArea();
    int const width = std::max(inner.width + 4, displayWidth(_title) + 4);
    return {.width = std::min(width, area.width), .height = std::min(inner.height + 2, area.height)};
}

void DialogImpl::paintFrame(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    canvas.fill(canvas.area(), ' ', theme.dialogBackground);
    canvas.drawBox(canvas.area(), ::core::tui::BorderStyle::Single, theme.dialogBorder, _title);
    arrangeStack(children(),
                 ::core::tui::Rect{.x = 2, .y = 1, .width = std::max(0, canvas.width() - 4),
                                   .height = std::max(0, canvas.height() - 2)},
                 StackSpec{.axis = ui::Axis::Vertical});
}

::core::tui::EventResult DialogImpl::frameEvent(::core::tui::InputEvent const& event) {
    if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event); key != nullptr && key->key == ::core::tui::KeyCode::Escape) {
        if (_onDismiss) {
            _onDismiss();
        }
        return ::core::tui::EventResult::Handled;
    }
    // A press on the box's own blank cells goes nowhere, and in particular not to the tree behind it.
    if (std::holds_alternative<::core::tui::MouseEvent>(event)) {
        return ::core::tui::EventResult::Handled;
    }
    return ::core::tui::EventResult::Ignored;
}

void DialogImpl::open() {
    if (_open) {
        return;
    }
    auto& screen = *context().screen;
    _restoreFocus = screen.focusedComponent();
    _open = true;
    context().openDialogs.push_back(_frame.get());
    screen.showOverlay(*_frame, centredIn(screen, frameSize()));
    std::vector<::core::tui::Component*> focusable;
    collectFocusable(*_frame, focusable);
    screen.setFocus(focusable.empty() ? static_cast<::core::tui::Component*>(_frame.get()) : focusable.front());
    refresh();
}

// The component focused before opening gets the focus back only while a widget still owns it: the tree behind
// may have been remounted in the meantime. No focus at all counts as focus inside: the mount destroys a dialog's
// content before it closes the dialog, and the focused content cleared the focus as it went.
void DialogImpl::close() {
    if (!_open) {
        return;
    }
    auto& screen = *context().screen;
    auto const* const focused = screen.focusedComponent();
    bool const focusInside = focused == nullptr || isWithin(focused, *_frame);
    screen.hideOverlay(*_frame);
    std::erase(context().openDialogs, _frame.get());
    _open = false;
    if (focusInside) {
        screen.setFocus(context().ownerOf(_restoreFocus) != nullptr ? _restoreFocus : nullptr);
    }
    _restoreFocus = nullptr;
    refresh();
}
```

Append to `src/tui/leaf_widgets.hpp` (inside the namespace; add `#include <cstdint>`):

```cpp
/// Busy: a spinner and a label while active, nothing otherwise. The frontend advances the frame on a timer while
/// any Busy is active.
class BusyImpl final : public TuiWidget<ui::BusyWidget> {
public:
    explicit BusyImpl(Context& context);
    ~BusyImpl() override;
    BusyImpl(BusyImpl const&) = delete;
    BusyImpl& operator=(BusyImpl const&) = delete;
    BusyImpl(BusyImpl&&) = delete;
    BusyImpl& operator=(BusyImpl&&) = delete;

    void setActive(bool active) override;
    void setLabel(std::string_view label) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::string probeText() const override { return _label; }
    void paint(::core::tui::Canvas& canvas) override;

private:
    bool _active = false;
    std::string _label;
};

/// Slider: `[====|----] value`; Left/Right move by step, Home/End to the ends.
class SliderImpl final : public TuiWidget<ui::SliderWidget> {
public:
    explicit SliderImpl(Context& context);
    void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) override;
    void setValue(std::int64_t value) override;
    void setOnChange(std::function<void(std::int64_t)> onChange) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;

private:
    void moveTo(std::int64_t value);

    std::int64_t _minimum = 0;
    std::int64_t _maximum = 100;
    std::int64_t _step = 1;
    std::int64_t _value = 0;
    std::function<void(std::int64_t)> _onChange;
};
```

Append to `src/tui/leaf_widgets.cpp` (inside the namespace; add `#include <array>`, `#include <core/tui/KeyCode.hpp>`,
`#include <core/tui/Modifier.hpp>` and `#include <string>`):

```cpp
namespace {

// ASCII frames: every terminal draws them, and a frame is one cell wide.
constexpr std::array<std::string_view, 4> kSpinner{"|", "/", "-", "\\"};

}  // namespace

BusyImpl::BusyImpl(Context& context) : TuiWidget{context} {
    adopt(std::make_unique<View>(*this));
}

BusyImpl::~BusyImpl() {
    if (_active) {
        --context().activeBusy;
    }
}

void BusyImpl::setActive(bool active) {
    if (active == _active) {
        return;
    }
    _active = active;
    if (active) {
        ++context().activeBusy;
    } else {
        --context().activeBusy;
    }
    refresh();
}

void BusyImpl::setLabel(std::string_view label) {
    _label = std::string{label};
    refresh();
}

::core::tui::Size BusyImpl::naturalSize() const {
    if (!_active) {
        return {.width = 0, .height = 0};
    }
    return {.width = 2 + displayWidth(_label), .height = 1};
}

void BusyImpl::paint(::core::tui::Canvas& canvas) {
    if (!_active) {
        return;
    }
    auto const frame = kSpinner.at(context().animationFrame % kSpinner.size());
    canvas.putString(0, 0, std::string{frame} + " " + _label, canvas.theme().textAccent);
}

SliderImpl::SliderImpl(Context& context) : TuiWidget{context} {
    adopt(std::make_unique<View>(*this));
}

void SliderImpl::setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) {
    _minimum = minimum;
    _maximum = std::max(minimum, maximum);
    _step = std::max<std::int64_t>(1, step);
    _value = std::clamp(_value, _minimum, _maximum);
    refresh();
}

void SliderImpl::setValue(std::int64_t value) {
    _value = std::clamp(value, _minimum, _maximum);
    refresh();
}

void SliderImpl::setOnChange(std::function<void(std::int64_t)> onChange) {
    _onChange = std::move(onChange);
}

::core::tui::Size SliderImpl::naturalSize() const {
    return {.width = 24, .height = 1};
}

std::string SliderImpl::probeText() const {
    return std::to_string(_value);
}

void SliderImpl::paint(::core::tui::Canvas& canvas) {
    auto const label = std::to_string(_value);
    int const track = std::max(3, canvas.width() - static_cast<int>(label.size()) - 3);
    auto const span = _maximum - _minimum;
    int const knob = span > 0 ? static_cast<int>((_value - _minimum) * (track - 1) / span) : 0;
    std::string bar = "[";
    for (int cell = 0; cell < track; ++cell) {
        bar += cell < knob ? "=" : cell == knob ? "|" : "-";
    }
    bar += "] " + label;
    auto const& theme = canvas.theme();
    canvas.putString(0, 0, bar, hasFocus() ? theme.buttonFocused : theme.textNormal);
}

::core::tui::EventResult SliderImpl::key(::core::tui::KeyEvent const& key) {
    if (::core::tui::withoutLockKeys(key.modifiers) != ::core::tui::Modifier::None) {
        return ::core::tui::EventResult::Ignored;
    }
    if (key.key == ::core::tui::KeyCode::Left) {
        moveTo(_value - _step);
    } else if (key.key == ::core::tui::KeyCode::Right) {
        moveTo(_value + _step);
    } else if (key.key == ::core::tui::KeyCode::Home) {
        moveTo(_minimum);
    } else if (key.key == ::core::tui::KeyCode::End) {
        moveTo(_maximum);
    } else {
        return ::core::tui::EventResult::Ignored;
    }
    return ::core::tui::EventResult::Handled;
}

void SliderImpl::moveTo(std::int64_t value) {
    auto const next = std::clamp(value, _minimum, _maximum);
    if (next == _value) {
        return;
    }
    _value = next;
    refresh();
    if (_onChange) {
        _onChange(next);
    }
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run:

```bash
cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[dialog],[busy],[slider]"
```
Expected: PASS, 8 test cases; and `"[tui]"` as a whole still passes.

Mutation checks, one at a time, each restored before the next:
- In `moveFocus`, delete the `if (context.openDialogs.empty())` test so the screen's walk always runs. Expected:
  FAIL in "Tab stays inside the open dialog" (the focus lands on "Behind", which the screen's walk reaches and the
  dialog's buttons are not part of).
- In `DialogImpl::close`, drop `focused == nullptr ||`. Expected: FAIL in "closing after its content is gone still
  hands the focus back" (nothing is focused).

- [ ] **Step 6: Commit**

```bash
git add src/tui tests/tui
git commit -m "wip(tui): dialog with a focus trap, busy spinner and slider

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: Table

**Files:**
- Create: `src/tui/table_widget.hpp`, `src/tui/table_widget.cpp`
- Modify: `CMakeLists.txt` — add both to `add_library(morph_tui STATIC …)`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_table.cpp`
- Test: `tests/tui/test_tui_table.cpp`

**Interfaces:**
- Consumes: `ui::TableWidget`, `ui::TableColumn{label, width}`, `ui::SelectionMode` (Part 2); `StackImpl::
  setColumnLayout` (Task 8); `layout::distribute` (Task 7).
- Produces: `TableImpl(Context&)`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_table.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/KeyCode.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/table_widget.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TableImpl;
using morph::tui::detail::TextImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

namespace {

ui::Key key(std::int64_t value) {
    return ui::Key{value};
}

/// A table of fruit: Name (content width) and Qty (fixed 5), rows keyed 1 and 2.
struct Fruit {
    explicit Fruit(Harness& harness, ui::SelectionMode mode) : table{harness.make<TableImpl>(nullptr)} {
        table->setColumns({{.label = "Name", .width = ui::Sizing::content()}, {.label = "Qty", .width = ui::Sizing::fixed(5)}});
        table->setSelectionMode(mode);
        addRow(harness, key(1), "apple", "3");
        addRow(harness, key(2), "kiwi", "12");
    }
    ~Fruit() {
        cells.clear();
        rows.clear();
    }
    Fruit(Fruit const&) = delete;
    Fruit& operator=(Fruit const&) = delete;
    Fruit(Fruit&&) = delete;
    Fruit& operator=(Fruit&&) = delete;

    void addRow(Harness& harness, ui::Key const& rowKey, std::string const& name, std::string const& quantity) {
        // As the mount builds a row: a horizontal stack, its cells, then the row's key.
        rows.push_back(harness.make<StackImpl>(table.get(), ui::Axis::Horizontal));
        for (auto const& text : {name, quantity}) {
            cells.push_back(harness.make<TextImpl>(rows.back().get()));
            cells.back()->setText(text);
        }
        table->setRowKey(*rows.back(), rowKey);
    }

    std::unique_ptr<TableImpl> table;
    std::vector<std::unique_ptr<StackImpl>> rows;
    std::vector<std::unique_ptr<TextImpl>> cells;
};

}  // namespace

TEST_CASE("tui table: a header row above keyed rows, in solved column widths", "[tui][table]") {
    Harness harness{30, 4};
    Fruit const fruit{harness, ui::SelectionMode::None};
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "  kiwi  12"});
}

TEST_CASE("tui table: Space toggles rows in Multiple mode, Enter activates", "[tui][table]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    std::vector<std::vector<ui::Key>> selections;
    std::vector<ui::Key> activated;
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
    fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
    harness.focus(*fruit.table);
    CHECK(harness.draw().at(1) == " >apple 3");
    static_cast<void>(harness.type(" "));
    CHECK(selections.back() == std::vector<ui::Key>{key(1)});
    CHECK(harness.draw().at(1) == "*>apple 3");
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.type(" "));
    CHECK(selections.back() == std::vector<ui::Key>{key(1), key(2)});
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(activated == std::vector<ui::Key>{key(2)});
    CHECK(selections.size() == 2);
}

TEST_CASE("tui table: Single mode selects one row; setSelection marks rows without reporting", "[tui][table]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Single};
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
    fruit.table->setSelection({key(2)});
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "* kiwi  12"});
    CHECK(selections.empty());
    harness.focus(*fruit.table);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(selections.back() == std::vector<ui::Key>{key(1)});
}

TEST_CASE("tui table: moving a row moves its key with it", "[tui][table]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Single};
    std::vector<ui::Key> activated;
    fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
    fruit.table->moveChild(*fruit.rows.at(1), 0);
    CHECK(harness.draw() == Rows{"  Name  Qty", "  kiwi  12", "  apple 3"});
    harness.focus(*fruit.table);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(activated == std::vector<ui::Key>{key(2)});
}
```

Add `test_tui_table.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'tui/table_widget.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/tui/table_widget.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <functional>
#include <morph/ui/backend.hpp>
#include <optional>
#include <unordered_map>
#include <vector>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Table: a header row above keyed rows. Each row is a horizontal stack of cells laid out in the column widths the
/// solver gives the table, one cell gap apart. A two-cell gutter marks selected rows `*` and, while the table has
/// focus, the cursor row `>`. Up/Down move the cursor; Space selects (Single) or toggles (Multiple); Enter selects
/// in Single mode and activates the cursor row.
class TableImpl final : public TuiContainer<ui::TableWidget> {
public:
    explicit TableImpl(Context& context);
    void setColumns(std::vector<ui::TableColumn> const& columns) override;
    void setSelectionMode(ui::SelectionMode mode) override;
    void setRowKey(ui::Widget& row, ui::Key const& key) override;
    void setSelection(std::vector<ui::Key> const& selection) override;
    void setOnSelectionChange(std::function<void(std::vector<ui::Key>)> onSelectionChange) override;
    void setOnActivate(std::function<void(ui::Key)> onActivate) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void click(::core::tui::Point cell) override;

protected:
    void childForgotten(WidgetBase& child) override;

private:
    [[nodiscard]] std::vector<int> naturalWidths() const;
    [[nodiscard]] std::optional<ui::Key> keyAt(std::size_t row) const;
    [[nodiscard]] bool isSelected(ui::Key const& key) const;
    void selectOnly(std::size_t row);
    void toggle(std::size_t row);
    void activateRow(std::size_t row);
    void report();

    std::vector<ui::TableColumn> _columns;
    ui::SelectionMode _mode = ui::SelectionMode::None;
    std::unordered_map<WidgetBase const*, ui::Key> _keys;
    std::vector<ui::Key> _selection;
    std::size_t _cursor = 0;
    std::function<void(std::vector<ui::Key>)> _onSelectionChange;
    std::function<void(ui::Key)> _onActivate;
};

}  // namespace morph::tui::detail
```

Create `src/tui/table_widget.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/table_widget.hpp"

#include <algorithm>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Theme.hpp>
#include <memory>
#include <numeric>
#include <string>
#include <utility>

#include "tui/container_widgets.hpp"
#include "tui/layout.hpp"

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

constexpr int kGutter = 2;
constexpr int kColumnGap = 1;

}  // namespace

TableImpl::TableImpl(Context& context) : TuiContainer{context} {
    adopt(std::make_unique<View>(*this));
}

void TableImpl::setColumns(std::vector<ui::TableColumn> const& columns) {
    _columns = columns;
    refresh();
}

void TableImpl::setSelectionMode(ui::SelectionMode mode) {
    _mode = mode;
    refresh();
}

void TableImpl::setRowKey(ui::Widget& row, ui::Key const& key) {
    _keys.insert_or_assign(&WidgetBase::of(row), key);
    refresh();
}

void TableImpl::setSelection(std::vector<ui::Key> const& selection) {
    _selection = selection;
    refresh();
}

void TableImpl::setOnSelectionChange(std::function<void(std::vector<ui::Key>)> onSelectionChange) {
    _onSelectionChange = std::move(onSelectionChange);
}

void TableImpl::setOnActivate(std::function<void(ui::Key)> onActivate) {
    _onActivate = std::move(onActivate);
}

void TableImpl::childForgotten(WidgetBase& child) {
    _keys.erase(&child);
    auto const count = shownChildren().size();
    _cursor = count == 0 ? 0 : std::min(_cursor, count - 1);
}

// Each column is as wide as its label or its widest cell.
std::vector<int> TableImpl::naturalWidths() const {
    std::vector<int> widths;
    auto const rows = shownChildren();
    for (std::size_t column = 0; column < _columns.size(); ++column) {
        int width = displayWidth(_columns.at(column).label);
        for (auto const* row : rows) {
            auto const* const stack = dynamic_cast<ContainerBase const*>(row);
            if (stack == nullptr) {
                continue;
            }
            auto const cells = stack->shownChildren();
            if (column < cells.size()) {
                width = std::max(width, cells.at(column)->naturalSize().width);
            }
        }
        widths.push_back(width);
    }
    return widths;
}

::core::tui::Size TableImpl::naturalSize() const {
    auto const widths = naturalWidths();
    int const width = kGutter + std::accumulate(widths.begin(), widths.end(), 0) +
                      (kColumnGap * std::max(0, static_cast<int>(widths.size()) - 1));
    int height = 1;
    for (auto const* row : shownChildren()) {
        height += std::max(1, row->naturalSize().height);
    }
    return {.width = width, .height = height};
}

std::optional<ui::Key> TableImpl::keyAt(std::size_t row) const {
    auto const rows = shownChildren();
    if (row >= rows.size()) {
        return std::nullopt;
    }
    auto const found = _keys.find(rows.at(row));
    return found == _keys.end() ? std::nullopt : std::optional{found->second};
}

bool TableImpl::isSelected(ui::Key const& key) const {
    return std::ranges::find(_selection, key) != _selection.end();
}

void TableImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto const natural = naturalWidths();
    std::vector<layout::Item> items;
    auto width = natural.begin();
    for (auto const& column : _columns) {
        items.push_back(layout::Item{.sizing = column.width, .natural = *width});
        ++width;
    }
    auto const widths =
        layout::distribute(items, layout::Track{.length = std::max(0, canvas.width() - kGutter), .gap = kColumnGap});

    int x = kGutter;
    auto columnWidth = widths.begin();
    for (auto const& column : _columns) {
        canvas.putString(0, x, column.label, theme.textBold);
        x += *columnWidth + kColumnGap;
        ++columnWidth;
    }

    int y = 1;
    std::size_t index = 0;
    for (auto* row : children()) {
        if (!row->shown()) {
            row->view().setArea({});
            continue;
        }
        if (auto* const stack = dynamic_cast<StackImpl*>(row)) {
            stack->setColumnLayout(widths, kColumnGap);
        }
        int const height = std::max(1, row->naturalSize().height);
        row->view().setArea({.x = kGutter, .y = y, .width = std::max(0, canvas.width() - kGutter), .height = height});
        auto const rowKey = keyAt(index);
        bool const cursor = hasFocus() && index == _cursor;
        std::string gutter{rowKey && isSelected(*rowKey) ? "*" : " "};
        gutter += cursor ? ">" : " ";
        canvas.putString(y, 0, gutter, cursor ? theme.listItemSelected : theme.textNormal);
        y += height;
        ++index;
    }
}

EventResult TableImpl::key(::core::tui::KeyEvent const& key) {
    if (::core::tui::withoutLockKeys(key.modifiers) != ::core::tui::Modifier::None) {
        return EventResult::Ignored;
    }
    auto const count = shownChildren().size();
    if (key.key == KeyCode::Up) {
        _cursor = _cursor > 0 ? _cursor - 1 : 0;
    } else if (key.key == KeyCode::Down) {
        _cursor = count == 0 ? 0 : std::min(_cursor + 1, count - 1);
    } else if (key.key == KeyCode::Home) {
        _cursor = 0;
    } else if (key.key == KeyCode::End) {
        _cursor = count == 0 ? 0 : count - 1;
    } else if (key.codepoint == U' ') {
        if (_mode == ui::SelectionMode::Single) {
            selectOnly(_cursor);
        } else if (_mode == ui::SelectionMode::Multiple) {
            toggle(_cursor);
        }
    } else if (key.key == KeyCode::Enter) {
        if (_mode == ui::SelectionMode::Single) {
            selectOnly(_cursor);
        }
        activateRow(_cursor);
    } else {
        return EventResult::Ignored;
    }
    refresh();
    return EventResult::Handled;
}

// A click on a row moves the cursor there and selects it (Single) or toggles it (Multiple).
void TableImpl::click(::core::tui::Point cell) {
    int y = 1;
    std::size_t index = 0;
    for (auto const* row : shownChildren()) {
        int const height = std::max(1, row->naturalSize().height);
        if (cell.y >= y && cell.y < y + height) {
            _cursor = index;
            if (_mode == ui::SelectionMode::Single) {
                selectOnly(index);
            } else if (_mode == ui::SelectionMode::Multiple) {
                toggle(index);
            }
            refresh();
            return;
        }
        y += height;
        ++index;
    }
}

void TableImpl::selectOnly(std::size_t row) {
    if (auto const rowKey = keyAt(row)) {
        _selection = {*rowKey};
        report();
    }
}

void TableImpl::toggle(std::size_t row) {
    auto const rowKey = keyAt(row);
    if (!rowKey) {
        return;
    }
    if (auto const found = std::ranges::find(_selection, *rowKey); found != _selection.end()) {
        _selection.erase(found);
    } else {
        _selection.push_back(*rowKey);
    }
    report();
}

void TableImpl::activateRow(std::size_t row) {
    if (auto const rowKey = keyAt(row); rowKey && _onActivate) {
        _onActivate(*rowKey);
    }
}

void TableImpl::report() {
    if (_onSelectionChange) {
        _onSelectionChange(_selection);
    }
}

}  // namespace morph::tui::detail
```

Add `src/tui/table_widget.cpp` and `src/tui/table_widget.hpp` to `add_library(morph_tui STATIC …)`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[table]"`
Expected: PASS, 4 test cases.

Mutation check: in `TableImpl::paint`, delete the `stack->setColumnLayout(widths, kColumnGap);` call. Expected: FAIL
in "a header row above keyed rows" (the row's cells pack together: `  apple3`). Restore.

- [ ] **Step 5: Commit**

```bash
git add src/tui/table_widget.hpp src/tui/table_widget.cpp tests/tui CMakeLists.txt
git commit -m "wip(tui): table with keyed rows and single or multiple selection

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 14: `tui::Backend`

**Files:**
- Create: `include/morph/tui/backend.hpp`, `src/tui/backend.cpp`
- Modify: `CMakeLists.txt` — `src/tui/backend.cpp` in `add_library(morph_tui STATIC …)`;
  `include/morph/tui/backend.hpp` in its `FILE_SET HEADERS`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_backend.cpp`
- Test: `tests/tui/test_tui_backend.cpp`

**Interfaces:**
- Consumes: `ui::IViewBackend` and its 19 factory signatures (Part 2, `include/morph/ui/backend.hpp`); every
  widget of Tasks 8–13; `detail::attach`, `fit`, `moveFocus` (Tasks 8, 12).
- Produces: `morph::tui::Backend(core::tui::Screen&)` implementing every `ui::IViewBackend` factory, plus
  `fit()`, `focusNext()`, `focusPrev()`, `focusFirst()`, `animating()`, `advanceAnimation()` — additions to the
  interface contract, in Part 3's own namespace.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_backend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <memory>
#include <morph/tui/backend.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/widget.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using morph::tui::Backend;
using morph::tui::detail::ContainerBase;
using morph::tui::testing::ResizableOutput;
using morph::tui::testing::rowsOf;

namespace {

/// A terminal, a screen over it and a backend over that.
struct Stage {
    explicit Stage(std::unique_ptr<core::tui::TerminalOutput> output)
        : terminal{std::move(output)}, screen{terminal}, backend{screen} {}
    core::tui::Terminal terminal;
    core::tui::Screen screen;
    Backend backend;
};

}  // namespace

TEST_CASE("tui::Backend: every factory appends one widget to its parent", "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(40, 12)};
    auto& backend = stage.backend;
    auto const column = backend.createStack(nullptr, ui::Axis::Vertical);
    std::vector<std::unique_ptr<ui::Widget>> made;
    made.push_back(backend.createText(column.get()));
    made.push_back(backend.createButton(column.get()));
    made.push_back(backend.createTextInput(column.get(), ui::TextInputMode::Multiline));
    made.push_back(backend.createCheckbox(column.get()));
    made.push_back(backend.createSelect(column.get(), ui::SelectStyle::Radio));
    made.push_back(backend.createSelect(column.get(), ui::SelectStyle::Dropdown));
    made.push_back(backend.createMenu(column.get()));
    made.push_back(backend.createStack(column.get(), ui::Axis::Horizontal));
    made.push_back(backend.createGrid(column.get()));
    made.push_back(backend.createSpacer(column.get()));
    made.push_back(backend.createPanel(column.get()));
    made.push_back(backend.createScroll(column.get(), ui::Axis::Vertical));
    made.push_back(backend.createSlot(column.get()));
    made.push_back(backend.createTabs(column.get()));
    made.push_back(backend.createDialog(column.get()));
    made.push_back(backend.createBusy(column.get()));
    made.push_back(backend.createTable(column.get()));
    made.push_back(backend.createDateTimeInput(column.get(), ui::DateMode::Date, 0));
    made.push_back(backend.createSlider(column.get()));
    made.push_back(backend.createFilePicker(column.get(), ui::FilePickerMode::Save));
    auto const& container = ContainerBase::of(*column);
    CHECK(container.children().size() == made.size());
    std::size_t index = 0;
    for (auto const* child : container.children()) {
        CHECK(&child->asWidget() == made.at(index).get());
        ++index;
    }
    backend.fit();
    stage.screen.draw();
    made.clear();
    CHECK(ContainerBase::of(*column).children().empty());
}

TEST_CASE("tui::Backend: fit re-fits the root to the screen after a resize", "[tui][backend]") {
    auto output = std::make_unique<ResizableOutput>(core::tui::Size{.width = 12, .height = 4});
    auto* const terminalOutput = output.get();
    Stage stage{std::move(output)};
    auto const panel = stage.backend.createPanel(nullptr);
    panel->setTitle("Box");
    stage.backend.fit();
    stage.screen.draw();
    CHECK(rowsOf(stage.screen).back() == "└──────────┘");

    terminalOutput->resize({.width = 16, .height = 5});
    static_cast<void>(stage.screen.dispatchEvent(core::tui::ResizeEvent{.columns = 16, .rows = 5}));
    stage.backend.fit();
    stage.screen.draw();
    auto const rows = rowsOf(stage.screen);
    CHECK(rows.size() == 5);
    CHECK(rows.back() == "└──────────────┘");
}

TEST_CASE("tui::Backend: animating while a Busy spins; advanceAnimation moves its frame", "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(20, 1)};
    auto const busy = stage.backend.createBusy(nullptr);
    busy->setLabel("Wait");
    CHECK_FALSE(stage.backend.animating());
    busy->setActive(true);
    CHECK(stage.backend.animating());
    stage.backend.fit();
    stage.screen.draw();
    CHECK(rowsOf(stage.screen) == std::vector<std::string>{"| Wait"});
    stage.backend.advanceAnimation();
    stage.screen.draw();
    CHECK(rowsOf(stage.screen) == std::vector<std::string>{"/ Wait"});
}

TEST_CASE("tui::Backend: focusFirst focuses the first focusable widget only when nothing has focus",
          "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(20, 3)};
    auto const column = stage.backend.createStack(nullptr, ui::Axis::Vertical);
    auto const label = stage.backend.createText(column.get());
    auto const first = stage.backend.createButton(column.get());
    auto const second = stage.backend.createButton(column.get());
    stage.backend.focusFirst();
    CHECK(stage.screen.focusedComponent() == &morph::tui::detail::WidgetBase::of(*first).view());
    stage.backend.focusNext();
    stage.backend.focusFirst();
    CHECK(stage.screen.focusedComponent() == &morph::tui::detail::WidgetBase::of(*second).view());
    stage.backend.focusPrev();
    CHECK(stage.screen.focusedComponent() == &morph::tui::detail::WidgetBase::of(*first).view());
}

TEST_CASE("tui::Backend: focusFirst moves into an open dialog whose content arrived after it opened",
          "[tui][backend]") {
    Stage stage{std::make_unique<core::tui::MockTerminalOutput>(30, 10)};
    auto const column = stage.backend.createStack(nullptr, ui::Axis::Vertical);
    auto const dialog = stage.backend.createDialog(column.get());
    dialog->setOpen(true);  // nothing inside yet: the frame holds the focus
    auto const body = stage.backend.createStack(dialog.get(), ui::Axis::Vertical);
    auto const okButton = stage.backend.createButton(body.get());
    stage.backend.focusFirst();
    CHECK(stage.screen.focusedComponent() == &morph::tui::detail::WidgetBase::of(*okButton).view());
}
```

Add `test_tui_backend.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'morph/tui/backend.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/tui/backend.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Screen.hpp>
#include <memory>

#include "../attributes.hpp"
#include "../ui/backend.hpp"

/// @file
/// @brief `morph::tui::Backend`: the terminal implementation of `ui::IViewBackend`.
///
/// Specified in `docs/spec/tui/frontend.md`, "Widgets".

namespace morph::tui {

/// @brief Builds every widget of a mounted view tree as a `core::tui::Component` on one `Screen`.
///
/// Containers arrange their children in cells when the screen renders them; a Dialog and an open Dropdown are
/// screen overlays; drag-and-drop runs on the mouse with pointer capture. The factories, documented on
/// `ui::IViewBackend`, run on the screen's thread. Widgets must be destroyed before the backend, and the backend
/// before the screen.
class Backend final : public ui::IViewBackend {
public:
    /// @param screen Where every widget renders. Borrowed: it must outlive the backend.
    explicit Backend(::core::tui::Screen& screen MORPH_LIFETIMEBOUND);
    /// @brief Destroys the backend; every widget it made must already be gone.
    ~Backend() override;
    Backend(Backend const&) = delete;
    Backend& operator=(Backend const&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    [[nodiscard]] std::unique_ptr<ui::TextWidget> createText(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::ButtonWidget> createButton(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TextInputWidget> createTextInput(ui::ContainerWidget* parent,
                                                                       ui::TextInputMode mode) override;
    [[nodiscard]] std::unique_ptr<ui::CheckboxWidget> createCheckbox(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::SelectWidget> createSelect(ui::ContainerWidget* parent,
                                                                 ui::SelectStyle style) override;
    [[nodiscard]] std::unique_ptr<ui::MenuWidget> createMenu(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::StackWidget> createStack(ui::ContainerWidget* parent, ui::Axis axis) override;
    [[nodiscard]] std::unique_ptr<ui::GridWidget> createGrid(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::SpacerWidget> createSpacer(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::PanelWidget> createPanel(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::ScrollWidget> createScroll(ui::ContainerWidget* parent, ui::Axis axis) override;
    [[nodiscard]] std::unique_ptr<ui::SlotWidget> createSlot(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TabsWidget> createTabs(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::DialogWidget> createDialog(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::BusyWidget> createBusy(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TableWidget> createTable(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::DateTimeInputWidget> createDateTimeInput(ui::ContainerWidget* parent,
                                                                               ui::DateMode mode,
                                                                               int offsetMinutes) override;
    [[nodiscard]] std::unique_ptr<ui::SliderWidget> createSlider(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::FilePickerWidget> createFilePicker(ui::ContainerWidget* parent,
                                                                         ui::FilePickerMode mode) override;

    /// @brief Sizes every root widget to the screen's viewport and centres every open dialog; call before each
    ///        `Screen::draw()`, which is how a resize reaches the tree.
    void fit();

    /// @brief Moves keyboard focus to the next focusable widget — inside the innermost open dialog when one is
    ///        open, because `Screen::focusNext` does not know overlays.
    void focusNext();

    /// @brief Moves keyboard focus to the previous focusable widget, with `focusNext`'s dialog rule.
    void focusPrev();

    /// @brief Focuses the first focusable widget when nothing has focus yet, or when the focus is on an open
    ///        dialog's frame (the dialog held nothing focusable when it opened, and has since gained something).
    void focusFirst();

    /// @brief Whether any Busy widget is spinning, so the frontend should keep advancing the animation.
    /// @return True while at least one Busy is active.
    [[nodiscard]] bool animating() const noexcept;

    /// @brief Moves every spinning Busy to its next frame; the next draw shows it.
    void advanceAnimation() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace morph::tui
```

Create `src/tui/backend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <morph/tui/backend.hpp>
#include <utility>

#include "tui/container_widgets.hpp"
#include "tui/context.hpp"
#include "tui/field_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/list_widgets.hpp"
#include "tui/table_widget.hpp"
#include "tui/widget.hpp"

namespace morph::tui {

struct Backend::Impl {
    explicit Impl(::core::tui::Screen& screen) : context{screen} {}
    detail::Context context;
};

namespace {

template <class WidgetType, class... Args>
std::unique_ptr<WidgetType> make(detail::Context& context, ui::ContainerWidget* parent, Args&&... args) {
    auto widget = std::make_unique<WidgetType>(context, std::forward<Args>(args)...);
    detail::attach(context, parent, *widget);
    return widget;
}

}  // namespace

Backend::Backend(::core::tui::Screen& screen) : _impl{std::make_unique<Impl>(screen)} {}

Backend::~Backend() = default;

std::unique_ptr<ui::TextWidget> Backend::createText(ui::ContainerWidget* parent) {
    return make<detail::TextImpl>(_impl->context, parent);
}

std::unique_ptr<ui::ButtonWidget> Backend::createButton(ui::ContainerWidget* parent) {
    return make<detail::ButtonImpl>(_impl->context, parent);
}

std::unique_ptr<ui::TextInputWidget> Backend::createTextInput(ui::ContainerWidget* parent, ui::TextInputMode mode) {
    return make<detail::TextInputImpl>(_impl->context, parent, mode);
}

std::unique_ptr<ui::CheckboxWidget> Backend::createCheckbox(ui::ContainerWidget* parent) {
    return make<detail::CheckboxImpl>(_impl->context, parent);
}

std::unique_ptr<ui::SelectWidget> Backend::createSelect(ui::ContainerWidget* parent, ui::SelectStyle style) {
    if (style == ui::SelectStyle::Radio) {
        return make<detail::RadioSelectImpl>(_impl->context, parent);
    }
    return make<detail::DropdownSelectImpl>(_impl->context, parent);
}

std::unique_ptr<ui::MenuWidget> Backend::createMenu(ui::ContainerWidget* parent) {
    return make<detail::MenuImpl>(_impl->context, parent);
}

std::unique_ptr<ui::StackWidget> Backend::createStack(ui::ContainerWidget* parent, ui::Axis axis) {
    return make<detail::StackImpl>(_impl->context, parent, axis);
}

std::unique_ptr<ui::GridWidget> Backend::createGrid(ui::ContainerWidget* parent) {
    return make<detail::GridImpl>(_impl->context, parent);
}

std::unique_ptr<ui::SpacerWidget> Backend::createSpacer(ui::ContainerWidget* parent) {
    return make<detail::SpacerImpl>(_impl->context, parent);
}

std::unique_ptr<ui::PanelWidget> Backend::createPanel(ui::ContainerWidget* parent) {
    return make<detail::PanelImpl>(_impl->context, parent);
}

std::unique_ptr<ui::ScrollWidget> Backend::createScroll(ui::ContainerWidget* parent, ui::Axis axis) {
    return make<detail::ScrollImpl>(_impl->context, parent, axis);
}

std::unique_ptr<ui::SlotWidget> Backend::createSlot(ui::ContainerWidget* parent) {
    return make<detail::SlotImpl>(_impl->context, parent);
}

std::unique_ptr<ui::TabsWidget> Backend::createTabs(ui::ContainerWidget* parent) {
    return make<detail::TabsImpl>(_impl->context, parent);
}

std::unique_ptr<ui::DialogWidget> Backend::createDialog(ui::ContainerWidget* parent) {
    return make<detail::DialogImpl>(_impl->context, parent);
}

std::unique_ptr<ui::BusyWidget> Backend::createBusy(ui::ContainerWidget* parent) {
    return make<detail::BusyImpl>(_impl->context, parent);
}

std::unique_ptr<ui::TableWidget> Backend::createTable(ui::ContainerWidget* parent) {
    return make<detail::TableImpl>(_impl->context, parent);
}

std::unique_ptr<ui::DateTimeInputWidget> Backend::createDateTimeInput(ui::ContainerWidget* parent, ui::DateMode mode,
                                                                      int offsetMinutes) {
    return make<detail::DateTimeInputImpl>(_impl->context, parent, mode, offsetMinutes);
}

std::unique_ptr<ui::SliderWidget> Backend::createSlider(ui::ContainerWidget* parent) {
    return make<detail::SliderImpl>(_impl->context, parent);
}

std::unique_ptr<ui::FilePickerWidget> Backend::createFilePicker(ui::ContainerWidget* parent, ui::FilePickerMode mode) {
    return make<detail::FilePickerImpl>(_impl->context, parent, mode);
}

void Backend::fit() {
    detail::fit(_impl->context);
}

void Backend::focusNext() {
    detail::moveFocus(_impl->context, detail::Direction::Forward);
}

void Backend::focusPrev() {
    detail::moveFocus(_impl->context, detail::Direction::Backward);
}

// Also when the focus is on an open dialog's frame: a dialog that held nothing focusable when it opened (a Switch
// inside it showing no case, say) could focus nothing but its frame, and focusable content has arrived since.
void Backend::focusFirst() {
    auto const& context = _impl->context;
    auto const* const focused = context.screen->focusedComponent();
    bool const onFrame = !context.openDialogs.empty() && focused == context.openDialogs.back();
    if (focused == nullptr || onFrame) {
        focusNext();
    }
}

bool Backend::animating() const noexcept {
    return _impl->context.activeBusy > 0;
}

void Backend::advanceAnimation() noexcept {
    ++_impl->context.animationFrame;
}

}  // namespace morph::tui
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build build/tui --target morph_tui_tests morph_verify_interface_header_sets && ./build/tui/tests/tui/morph_tui_tests "[backend]"
```
Expected: PASS, 5 test cases; `backend.hpp` compiles standalone.

Mutation check: in `Backend::focusFirst`, drop the `== nullptr` test so it always calls `focusNext()`. Expected:
FAIL in "focusFirst focuses the first focusable widget only when nothing has focus" (the second `focusFirst` moves
the focus to the next widget, which wraps to the first). Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/tui/backend.hpp src/tui/backend.cpp tests/tui CMakeLists.txt
git commit -m "wip(tui): tui::Backend, every ui::IViewBackend factory

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 15: Drag-and-drop

**Files:**
- Create: `src/tui/drag.hpp`, `src/tui/drag.cpp`
- Modify: `src/tui/context.hpp` — forward-declare `class DragController;`, add the member
  `std::unique_ptr<DragController> drag;` as the **last** member of `Context` (and `#include <memory>`)
- Modify: `src/tui/context.cpp` — the constructor and `forget` (below)
- Modify: `src/tui/widget.cpp` — replace `WidgetBase::pointer` (below); `#include "tui/drag.hpp"`
- Modify: `CMakeLists.txt` — add both to `add_library(morph_tui STATIC …)`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_drag.cpp`
- Test: `tests/tui/test_tui_drag.cpp`

**Interfaces:**
- Consumes: `Screen::componentAt(int row, int col) const` (public since core-cpp 0.7), `Screen::releasePointer()`,
  implicit pointer capture (spec 0 §2), `Screen::showOverlay`/`hideOverlay`/`isOverlayVisible`;
  `core::tui::VtParser::feed` (core-cpp `tui/VtParser.hpp`, used by the test harness); `WidgetBase::dragKey()`,
  `accepts`, `drop`, `isDropTarget` (Task 8).
- Produces: `DragController(Context&)` with `press(WidgetBase&, ui::Key, Point)`, `move(Point)`,
  `release(Point) -> bool`, `forget(WidgetBase const&)`, `source()`, `target()`, `dragging()`; `Context::drag`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_drag.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Drag-and-drop driven by SGR mouse reports, decoded by core-cpp's parser:
// CSI < b ; x ; y M is a press (b = 0) or, with bit 32 set, a motion with the
// button held; a final m is the release. Coordinates are 1-based.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/drag.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/widget.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::detail::WidgetBase;
using morph::tui::testing::Harness;

namespace {

ui::Key key(std::int64_t value) {
    return ui::Key{value};
}

/// Two 10-cell columns two cells apart; "task" in the first carries key 7.
struct Board {
    explicit Board(Harness& harness)
        : board{harness.make<StackImpl>(nullptr, ui::Axis::Horizontal)},
          todo{harness.make<StackImpl>(board.get(), ui::Axis::Vertical)},
          done{harness.make<StackImpl>(board.get(), ui::Axis::Vertical)},
          card{harness.make<TextImpl>(todo.get())} {
        board->setGap(2);
        todo->setLayout({.width = ui::Sizing::fixed(10), .height = {}});
        done->setLayout({.width = ui::Sizing::fixed(10), .height = {}});
        card->setText("task");
        card->setDragKey(key(7));
    }

    std::unique_ptr<StackImpl> board;
    std::unique_ptr<StackImpl> todo;
    std::unique_ptr<StackImpl> done;
    std::unique_ptr<TextImpl> card;
};

bool contains(std::vector<std::string> const& rows, std::string const& text) {
    for (auto const& row : rows) {
        if (row.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("tui drag: a card dragged onto an accepting column is dropped there", "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    std::vector<ui::Key> dropped;
    // An accept-everything predicate, as the mount passes for a node without `accepts`.
    board.done->setDropHandler([](ui::Key const&) { return true; },
                               [&](ui::Key dropKey) { dropped.push_back(std::move(dropKey)); });
    static_cast<void>(harness.draw());

    harness.sgr("\x1b[<0;1;1M");    // press on the card
    harness.sgr("\x1b[<32;14;1M");  // move over the second column, button held
    auto const during = harness.draw();
    CHECK(contains(during, "[task]"));
    CHECK(contains(during, "╔"));
    CHECK(harness.context().drag->target() == &WidgetBase::of(*board.done));

    harness.sgr("\x1b[<0;14;1m");  // release
    CHECK(dropped == std::vector<ui::Key>{key(7)});
    auto const after = harness.draw();
    CHECK_FALSE(contains(after, "[task]"));
    CHECK_FALSE(contains(after, "╔"));
}

TEST_CASE("tui drag: a target whose accepts refuses the key is not highlighted and gets no drop", "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    int dropped = 0;
    board.done->setDropHandler([](ui::Key const&) { return false; }, [&](ui::Key const&) { ++dropped; });
    static_cast<void>(harness.draw());
    harness.sgr("\x1b[<0;1;1M");
    harness.sgr("\x1b[<32;14;1M");
    CHECK(harness.context().drag->target() == nullptr);
    harness.sgr("\x1b[<0;14;1m");
    CHECK(dropped == 0);
}

TEST_CASE("tui drag: a press and release without motion is a click, not a drag", "[tui][drag]") {
    Harness harness{30, 2};
    auto const button = harness.make<ButtonImpl>(nullptr);
    int clicks = 0;
    button->setLabel("Card");
    button->setDragKey(key(3));
    button->setOnClick([&] { ++clicks; });
    static_cast<void>(harness.draw());
    harness.sgr("\x1b[<0;2;1M");
    harness.sgr("\x1b[<0;2;1m");
    CHECK(clicks == 1);
    CHECK_FALSE(harness.context().drag->dragging());
}

TEST_CASE("tui drag: destroying the drag source mid-drag ends the gesture", "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    int dropped = 0;
    board.done->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++dropped; });
    static_cast<void>(harness.draw());
    harness.sgr("\x1b[<0;1;1M");
    harness.sgr("\x1b[<32;14;1M");
    REQUIRE(harness.context().drag->dragging());

    board.card.reset();  // the card was moved elsewhere and remounted
    CHECK(harness.context().drag->source() == nullptr);
    CHECK_FALSE(harness.context().drag->dragging());
    auto const rows = harness.draw();
    CHECK_FALSE(contains(rows, "[task]"));
    CHECK_FALSE(contains(rows, "╔"));
    harness.sgr("\x1b[<0;14;1m");
    CHECK(dropped == 0);
}
```

Add `test_tui_drag.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'tui/drag.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/tui/drag.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Rect.hpp>
#include <memory>
#include <morph/ui/view.hpp>
#include <optional>

#include "tui/context.hpp"

namespace morph::tui::detail {

class WidgetBase;

/// The one drag gesture a backend can have at a time. A press on a widget with a drag key arms it; the first
/// motion a cell away starts the drag and shows a label beside the pointer; the drop target is the first widget,
/// from the one under the pointer up through its parents, that accepts the key, and it is outlined; the release
/// drops the key there. Pointer capture (core::tui) sends the whole gesture to the source.
class DragController {
public:
    explicit DragController(Context& context);
    ~DragController();
    DragController(DragController const&) = delete;
    DragController& operator=(DragController const&) = delete;
    DragController(DragController&&) = delete;
    DragController& operator=(DragController&&) = delete;

    /// A press on @p source, at viewport cell @p point.
    void press(WidgetBase& source, ui::Key key, ::core::tui::Point point);
    /// The pointer moved to @p point with the button held.
    void move(::core::tui::Point point);
    /// The button was released at @p point; true when the gesture was a drag, so the release is not a click.
    [[nodiscard]] bool release(::core::tui::Point point);
    /// A widget is being destroyed: a source ends the gesture and releases the pointer; a target stops being one.
    void forget(WidgetBase const& widget);

    [[nodiscard]] WidgetBase const* source() const noexcept { return _source; }
    [[nodiscard]] WidgetBase const* target() const noexcept { return _target; }
    [[nodiscard]] bool dragging() const noexcept { return _dragging; }

private:
    class Label;
    class Highlight;

    [[nodiscard]] WidgetBase* findTarget(::core::tui::Point point) const;
    void setTarget(WidgetBase* target);
    void showLabel(::core::tui::Point point);
    void end();

    Context* _context;
    WidgetBase* _source = nullptr;
    std::optional<ui::Key> _key;
    ::core::tui::Point _pressedAt{};
    ::core::tui::Point _labelAt{};
    bool _dragging = false;
    WidgetBase* _target = nullptr;
    std::unique_ptr<Label> _label;
    std::unique_ptr<Highlight> _highlight;
};

}  // namespace morph::tui::detail
```

Create `src/tui/drag.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/drag.hpp"

#include <core/tui/Box.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Theme.hpp>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "tui/widget.hpp"

namespace morph::tui::detail {

namespace {

/// Cells the pointer travels from the press before the press becomes a drag.
constexpr int kDragThreshold = 1;
/// Columns between the pointer and the label, so the label never lies under the pointer.
constexpr int kLabelOffset = 2;

std::string keyText(ui::Key const& key) {
    return std::visit(
        [](auto const& value) -> std::string {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>) {
                return value;
            } else {
                return std::to_string(value);
            }
        },
        key);
}

}  // namespace

/// The label that follows the pointer: the source's first line of text, or its key.
class DragController::Label final : public ::core::tui::Component {
public:
    void setText(std::string text) { _text = std::move(text); }
    void render(::core::tui::Canvas& canvas) override { canvas.putString(0, 0, _text, canvas.theme().listItemSelected); }
    [[nodiscard]] ::core::tui::Size preferredSize() const override { return {.width = displayWidth(_text), .height = 1}; }

private:
    std::string _text;
};

/// The outline drawn over the drop target. An overlay, so the target's children cannot paint over it.
class DragController::Highlight final : public ::core::tui::Component {
public:
    void setSize(::core::tui::Size size) { _size = size; }
    void render(::core::tui::Canvas& canvas) override {
        canvas.drawBox(canvas.area(), ::core::tui::BorderStyle::Double, canvas.theme().textAccent);
    }
    [[nodiscard]] ::core::tui::Size preferredSize() const override { return _size; }

private:
    ::core::tui::Size _size{};
};

DragController::DragController(Context& context)
    : _context{&context}, _label{std::make_unique<Label>()}, _highlight{std::make_unique<Highlight>()} {}

// NOLINTNEXTLINE(bugprone-exception-escape): hiding the overlays cannot be skipped; the screen would keep them.
DragController::~DragController() {
    end();
}

void DragController::press(WidgetBase& source, ui::Key key, ::core::tui::Point point) {
    end();
    _source = &source;
    _key = std::move(key);
    _pressedAt = point;
}

void DragController::move(::core::tui::Point point) {
    if (_source == nullptr || !_key) {
        return;
    }
    if (!_dragging) {
        if (std::abs(point.x - _pressedAt.x) + std::abs(point.y - _pressedAt.y) < kDragThreshold) {
            return;
        }
        _dragging = true;
        auto const lines = splitLines(_source->probeText());
        auto const text = lines.front().empty() ? keyText(*_key) : std::string{lines.front()};
        _label->setText("[" + text + "]");
    }
    showLabel(point);
    setTarget(findTarget(point));
}

bool DragController::release(::core::tui::Point point) {
    if (_source == nullptr) {
        return false;
    }
    bool const dragged = _dragging;
    auto* const target = dragged ? findTarget(point) : nullptr;
    auto const key = _key;
    end();
    // After end(): the overlays are gone before the application hears of the drop.
    if (target != nullptr && key) {
        target->drop(*key);
    }
    return dragged;
}

void DragController::forget(WidgetBase const& widget) {
    if (&widget == _source) {
        end();
        _context->screen->releasePointer();
        return;
    }
    if (&widget == _target) {
        setTarget(nullptr);
    }
}

WidgetBase* DragController::findTarget(::core::tui::Point point) const {
    if (!_key) {
        return nullptr;
    }
    auto const* const hit = _context->screen->componentAt(point.y, point.x);
    // The outline covers the target exactly, so a pointer over it is still over the target.
    if (hit == _highlight.get() || hit == _label.get()) {
        return _target;
    }
    for (auto const* node = hit; node != nullptr; node = node->parent()) {
        auto* const widget = _context->ownerOf(node);
        if (widget != nullptr && widget != _source && widget->accepts(*_key)) {
            return widget;
        }
    }
    return nullptr;
}

void DragController::setTarget(WidgetBase* target) {
    if (target == _target) {
        return;
    }
    auto& screen = *_context->screen;
    _target = target;
    if (target == nullptr) {
        screen.hideOverlay(*_highlight);
        return;
    }
    auto const bounds = target->view().screenBounds();
    _highlight->setSize(bounds.size());
    screen.showOverlay(*_highlight, bounds.position());
    // The label stays on top: shown again, it becomes the last overlay drawn.
    if (screen.isOverlayVisible(*_label)) {
        screen.hideOverlay(*_label);
        screen.showOverlay(*_label, _labelAt);
    }
}

void DragController::showLabel(::core::tui::Point point) {
    _labelAt = ::core::tui::Point{.x = point.x + kLabelOffset, .y = point.y};
    _context->screen->showOverlay(*_label, _labelAt);
}

void DragController::end() {
    auto& screen = *_context->screen;
    screen.hideOverlay(*_label);
    screen.hideOverlay(*_highlight);
    _source = nullptr;
    _target = nullptr;
    _key.reset();
    _dragging = false;
}

}  // namespace morph::tui::detail
```

In `src/tui/context.cpp`, add `#include "tui/drag.hpp"` and `#include <memory>`, and replace the constructor and
`forget` with:

```cpp
Context::Context(::core::tui::Screen& target) : screen{&target}, drag{std::make_unique<DragController>(*this)} {}

void Context::forget(WidgetBase& widget) {
    std::erase(roots, &widget);
    drag->forget(widget);
}
```

(`Context::~Context() = default;` stays in `context.cpp`, where `DragController` is complete.) Document the new
member in `context.hpp`: `std::unique_ptr<DragController> drag;  ///< The one drag gesture; last, so it is destroyed
first and hides its overlays while the screen is still there.`

In `src/tui/widget.cpp`, replace `WidgetBase::pointer` with:

```cpp
EventResult WidgetBase::pointer(::core::tui::MouseEvent const& mouse) {
    using Type = ::core::tui::MouseEvent::Type;
    if (blockedByDialog()) {
        return EventResult::Handled;
    }
    if (mouse.type == Type::ScrollUp || mouse.type == Type::ScrollDown) {
        return wheel(mouse.type == Type::ScrollUp ? -1 : 1) ? EventResult::Handled : EventResult::Ignored;
    }
    auto const bounds = _view->screenBounds();
    ::core::tui::Point const cell{.x = mouse.x - 1, .y = mouse.y - 1};
    ::core::tui::Point const point{.x = bounds.x + cell.x, .y = bounds.y + cell.y};
    auto& drag = *_context->drag;
    if (mouse.type == Type::Press) {
        if (mouse.button != 0 || !(focusable() || _dragKey)) {
            return EventResult::Ignored;
        }
        _pressed = true;
        if (focusable()) {
            _context->screen->setFocus(_view.get());
        }
        if (_dragKey) {
            drag.press(*this, *_dragKey, point);
        }
        return EventResult::Handled;
    }
    if (!_pressed) {
        return EventResult::Ignored;
    }
    if (mouse.type == Type::Move) {
        if (drag.source() == this) {
            drag.move(point);
        }
        return EventResult::Handled;
    }
    if (mouse.type == Type::Release) {
        _pressed = false;
        bool const dragged = drag.source() == this && drag.release(point);
        bool const inside = cell.x >= 0 && cell.y >= 0 && cell.x < bounds.width && cell.y < bounds.height;
        if (!dragged && inside && _enabled) {
            click(cell);
        }
    }
    return EventResult::Handled;
}
```

Add `src/tui/drag.cpp` and `src/tui/drag.hpp` to `add_library(morph_tui STATIC …)`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[drag]"`
Expected: PASS, 4 test cases; then `"[tui]"` as a whole.

Mutation check: in `Context::forget`, delete `drag->forget(widget);`. Expected: FAIL in "destroying the drag source
mid-drag ends the gesture" (`source()` still names the freed card; under ASan the release that follows is a
use-after-free). Restore. Then in `DragController::move` change `kDragThreshold` to 100. Expected: FAIL in "a card
dragged onto an accepting column is dropped there". Restore.

- [ ] **Step 5: Commit**

```bash
git add src/tui tests/tui CMakeLists.txt
git commit -m "wip(tui): drag-and-drop with pointer capture, a label and a drop outline

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 16: `LoopScheduler`

**Files:**
- Create: `src/tui/scheduler.hpp`, `src/tui/scheduler.cpp`
- Modify: `CMakeLists.txt` — add both to `add_library(morph_tui STATIC …)`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_scheduler.cpp`
- Test: `tests/tui/test_tui_scheduler.cpp`

**Interfaces:**
- Consumes: `reactive::Scheduler`, `reactive::TimerHandle` (Part 1, `include/morph/reactive/scheduler.hpp`);
  `EventLoop::addTimer(SteadyTimePoint, TimerCallback, void*)`, `cancelTimer(TimerId)`, `clock()`;
  `IoLoop(IoLoopDriver::Caller)` (Task 1); `LoopExecutor` (Task 5).
- Produces: `detail::LoopScheduler(core::net::EventLoop&, exec::IExecutor& owner)` with `after`, `every`,
  `pendingTimers()`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_scheduler.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/tui/loop_executor.hpp>
#include <stdexcept>

#include "tui/scheduler.hpp"

using morph::reactive::TimerHandle;
using morph::tui::LoopExecutor;
using morph::tui::detail::LoopScheduler;
using namespace std::chrono_literals;

namespace {

/// A caller-driven loop, its executor, and a scheduler on both.
struct Loop {
    morph::exec::IoLoop io{morph::exec::IoLoopDriver::Caller};
    LoopExecutor executor{io.loop()};
    LoopScheduler scheduler{io.loop(), executor};

    /// Turns the loop on this thread for @p duration.
    void turnFor(std::chrono::milliseconds duration) {
        auto const deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(io.loop().runOnce(2ms));
        }
    }

    /// Turns the loop until @p done holds or two seconds pass.
    template <class Pred>
    bool turnUntil(Pred done) {
        auto const deadline = std::chrono::steady_clock::now() + 2s;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            static_cast<void>(io.loop().runOnce(2ms));
        }
        return true;
    }
};

}  // namespace

TEST_CASE("tui::LoopScheduler: after fires once, in a loop turn, with the owner current", "[tui][scheduler]") {
    Loop loop;
    int fired = 0;
    bool onOwner = false;
    TimerHandle const handle = loop.scheduler.after(5ms, [&] {
        ++fired;
        onOwner = morph::exec::runningOn(loop.executor);
    });
    REQUIRE(loop.turnUntil([&] { return fired == 1; }));
    loop.turnFor(20ms);
    CHECK(fired == 1);
    CHECK(onOwner);
    CHECK(loop.scheduler.pendingTimers() == 0);
}

TEST_CASE("tui::LoopScheduler: every repeats until its handle is cancelled", "[tui][scheduler]") {
    Loop loop;
    int fired = 0;
    TimerHandle handle = loop.scheduler.every(3ms, [&] { ++fired; });
    REQUIRE(loop.turnUntil([&] { return fired >= 3; }));
    handle.cancel();
    int const atCancel = fired;
    loop.turnFor(20ms);
    CHECK(fired == atCancel);
    CHECK(loop.scheduler.pendingTimers() == 0);
}

TEST_CASE("tui::LoopScheduler: destroying the handle cancels the timer", "[tui][scheduler]") {
    Loop loop;
    int fired = 0;
    {
        TimerHandle const transient = loop.scheduler.after(2ms, [&] { ++fired; });
    }
    loop.turnFor(20ms);
    CHECK(fired == 0);
    CHECK(loop.scheduler.pendingTimers() == 0);
}

TEST_CASE("tui::LoopScheduler: a callback may cancel its own timer", "[tui][scheduler]") {
    Loop loop;
    int fired = 0;
    TimerHandle handle;
    handle = loop.scheduler.every(2ms, [&] {
        ++fired;
        handle.cancel();
    });
    loop.turnFor(30ms);
    CHECK(fired == 1);
}

TEST_CASE("tui::LoopScheduler: a handle outliving its scheduler is safe", "[tui][scheduler]") {
    TimerHandle handle;
    {
        Loop loop;
        handle = loop.scheduler.every(10ms, [] {});
    }
    handle.cancel();
    CHECK_FALSE(handle.active());
}

TEST_CASE("tui::LoopScheduler: every refuses a non-positive period", "[tui][scheduler]") {
    Loop loop;
    CHECK_THROWS_AS(static_cast<void>(loop.scheduler.every(0ms, [] {})), std::invalid_argument);
}
```

Add `test_tui_scheduler.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'tui/scheduler.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/tui/scheduler.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/net/EventLoop.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/reactive/scheduler.hpp>

namespace morph::tui::detail {

/// reactive::Scheduler on a core::net::EventLoop's timers: each callback runs in a loop turn with the owner
/// executor current, so it may write the reactive state of a runtime that executor owns. Used on the loop's
/// thread only.
class LoopScheduler final : public reactive::Scheduler {
public:
    LoopScheduler(::core::net::EventLoop& loop, exec::IExecutor& owner);
    ~LoopScheduler() override;
    LoopScheduler(LoopScheduler const&) = delete;
    LoopScheduler& operator=(LoopScheduler const&) = delete;
    LoopScheduler(LoopScheduler&&) = delete;
    LoopScheduler& operator=(LoopScheduler&&) = delete;

    [[nodiscard]] reactive::TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) override;
    [[nodiscard]] reactive::TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) override;
    /// How many timers are armed.
    [[nodiscard]] std::size_t pendingTimers() const noexcept;

private:
    struct State;
    [[nodiscard]] reactive::TimerHandle add(std::chrono::milliseconds delay, std::chrono::milliseconds period,
                                            std::function<void()> fn);

    /// Shared with each handle's cancel, which holds it weakly: a handle may outlive the scheduler.
    std::shared_ptr<State> _state;
};

}  // namespace morph::tui::detail
```

Create `src/tui/scheduler.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/scheduler.hpp"

#include <core/async/ExecutorContext.hpp>
#include <cstdint>
#include <exception>
#include <morph/core/logger.hpp>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace morph::tui::detail {

struct LoopScheduler::State {
    /// One armed timer. Held by unique_ptr, so its address — the loop timer's state — never moves.
    struct Timer {
        State* state = nullptr;
        std::uint64_t id = 0;
        ::core::net::TimerId timer{};
        std::chrono::milliseconds period{0};
        std::function<void()> fn;
    };

    State(::core::net::EventLoop& eventLoop, exec::IExecutor& executor) : loop{&eventLoop}, owner{&executor} {}

    void arm(Timer& timer, std::chrono::milliseconds delay) {
        timer.timer = loop->addTimer(loop->clock().now() + delay, &State::fire, &timer);
    }

    void cancel(std::uint64_t timerId) {
        auto const found = timers.find(timerId);
        if (found == timers.end()) {
            return;
        }
        static_cast<void>(loop->cancelTimer(found->second->timer));
        timers.erase(found);
    }

    void cancelAll() {
        for (auto const& entry : timers) {
            static_cast<void>(loop->cancelTimer(entry.second->timer));
        }
        timers.clear();
    }

    /// The loop's timer callback. The callback is copied out before it runs, because it may cancel its own timer
    /// or destroy the scheduler; nothing here touches the state after it returns.
    static void fire(void* armed) {
        auto* const timer = static_cast<Timer*>(armed);
        State& state = *timer->state;
        std::function<void()> const fn = timer->fn;
        if (timer->period.count() > 0) {
            state.arm(*timer, timer->period);
        } else {
            state.timers.erase(timer->id);
        }
        ::core::async::ExecutorScope const scope{state.owner->coreExecutor()};
        try {
            fn();
        } catch (std::exception const& failure) {
            ::morph::log::logError("[tui] timer callback threw: {}", failure.what());
        } catch (...) {
            ::morph::log::logError("[tui] timer callback threw an unknown exception");
        }
    }

    ::core::net::EventLoop* loop;
    exec::IExecutor* owner;
    std::uint64_t nextId = 0;
    std::unordered_map<std::uint64_t, std::unique_ptr<Timer>> timers;
};

LoopScheduler::LoopScheduler(::core::net::EventLoop& loop, exec::IExecutor& owner)
    : _state{std::make_shared<State>(loop, owner)} {}

// NOLINTNEXTLINE(bugprone-exception-escape): every armed timer names this state; none may outlive it.
LoopScheduler::~LoopScheduler() {
    _state->cancelAll();
}

reactive::TimerHandle LoopScheduler::after(std::chrono::milliseconds delay, std::function<void()> fn) {
    return add(delay, std::chrono::milliseconds{0}, std::move(fn));
}

reactive::TimerHandle LoopScheduler::every(std::chrono::milliseconds period, std::function<void()> fn) {
    if (period.count() <= 0) {
        throw std::invalid_argument{"morph::tui: Scheduler::every needs a positive period"};
    }
    return add(period, period, std::move(fn));
}

std::size_t LoopScheduler::pendingTimers() const noexcept {
    return _state->timers.size();
}

reactive::TimerHandle LoopScheduler::add(std::chrono::milliseconds delay, std::chrono::milliseconds period,
                                         std::function<void()> fn) {
    auto const timerId = ++_state->nextId;
    auto timer = std::make_unique<State::Timer>(
        State::Timer{.state = _state.get(), .id = timerId, .timer = {}, .period = period, .fn = std::move(fn)});
    auto& armed = *timer;
    _state->timers.emplace(timerId, std::move(timer));
    _state->arm(armed, delay);
    return reactive::TimerHandle{[weak = std::weak_ptr<State>{_state}, timerId] {
        if (auto const state = weak.lock()) {
            state->cancel(timerId);
        }
    }};
}

}  // namespace morph::tui::detail
```

Add `src/tui/scheduler.cpp` and `src/tui/scheduler.hpp` to `add_library(morph_tui STATIC …)`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[scheduler]"`
Expected: PASS, 6 test cases.

Mutation check: in `State::fire`, delete the re-arm (`state.arm(*timer, timer->period);`) and erase every timer
after it fires. Expected: FAIL in "every repeats until its handle is cancelled" (`turnUntil` gives up at one).
Restore.

- [ ] **Step 5: Commit**

```bash
git add src/tui/scheduler.hpp src/tui/scheduler.cpp tests/tui CMakeLists.txt
git commit -m "wip(tui): LoopScheduler, the frontend's timers on the event loop

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 17: `tui::Frontend` and the event pump

**Files:**
- Create: `include/morph/tui/frontend.hpp`, `src/tui/frontend.cpp`, `src/tui/session.hpp`, `src/tui/session.cpp`
- Modify: `CMakeLists.txt` — the two `.cpp` and `session.hpp` in `add_library(morph_tui STATIC …)`;
  `include/morph/tui/frontend.hpp` in its `FILE_SET HEADERS`
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_frontend.cpp`
- Test: `tests/tui/test_tui_frontend.cpp`

**Interfaces:**
- Consumes: `ui::Frontend`, `ui::AppContext`, `ui::Application`, `ui::ApplicationFactory`, `ui::FrontendOption`,
  `ui::Mounted(reactive::Runtime&, IViewBackend&, Node, ContainerWidget* = nullptr)` (Part 2);
  `reactive::Runtime(IExecutor&, RuntimeOptions{.afterFlush})` (Part 1); `IoLoop{IoLoopDriver::Caller}` (Task 1);
  `LoopExecutor` (Task 5); `Backend` with `fit`, `focusNext`, `focusPrev`, `focusFirst`, `animating`,
  `advanceAnimation` (Task 14); `LoopScheduler` (Task 16); core-cpp `TuiRuntime(EventLoop&, InputSource&)` /
  `(EventLoop&, Terminal&)`, `blockOn`, `nextEvent`, `inputClosed`, `setInterruptHandler`;
  `core::async::{Task, whenAny, AsyncQueue, OperationCancelled, ExecutorScope}`; `Terminal::setMouseTracking`,
  `Terminal::initialize`; `core::platform::{isTerminal, standardInput}`.
- Produces: `morph::tui::FrontendConfig{terminal, input, mouse}`, `morph::tui::Frontend(FrontendConfig)` with
  `name()` = `"tui"` and `run(ApplicationFactory const&) -> int`, `morph::tui::frontendOption(FrontendConfig) ->
  ui::FrontendOption` — exactly the contract's Part 3 names; private `detail::Session`.

- [ ] **Step 1: Write the failing test**

Create `tests/tui/test_tui_frontend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/platform/SystemPipe.hpp>
#include <core/platform/Types.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/runtime/testing/ScriptedInputSource.hpp>
#include <functional>
#include <memory>
#include <morph/reactive/detail/graph.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/tui/frontend.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <utility>
#include <vector>

#include "../owner_probe_recorder.hpp"
#include "../test_support.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using core::tui::runtime::testing::ScriptedInputSource;
using morph::tui::testing::ResizableOutput;
using namespace std::chrono_literals;

namespace {

core::tui::InputEvent key(char character, core::tui::Modifier modifiers = core::tui::Modifier::None) {
    return core::tui::test::charKey(character, modifiers);
}

core::tui::InputEvent special(KeyCode code, core::tui::Modifier modifiers = core::tui::Modifier::None) {
    return core::tui::test::specialKey(code, modifiers);
}

std::unique_ptr<core::platform::SystemPipe> openPipe() {
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());
    return std::move(*pipe);
}

/// A frontend over scripted input and a frame-counting mock terminal.
struct Rig {
    explicit Rig(core::tui::Size size) : pipe{openPipe()}, source{pipe.get()}, terminal{makeOutput(size)} {}

    [[nodiscard]] morph::tui::Frontend frontend() {
        return morph::tui::Frontend{morph::tui::FrontendConfig{.terminal = &terminal, .input = &source}};
    }

    std::unique_ptr<core::platform::SystemPipe> pipe;
    ScriptedInputSource source;
    ResizableOutput* output = nullptr;
    core::tui::Terminal terminal;

private:
    std::unique_ptr<core::tui::TerminalOutput> makeOutput(core::tui::Size size) {
        auto made = std::make_unique<ResizableOutput>(size);
        output = made.get();
        return made;
    }
};

/// An application whose view a lambda builds; it keeps a timer and a counter for the test.
struct TestApp final : ui::Application {
    explicit TestApp(morph::reactive::Runtime& runtime) : count{runtime, 0} {}
    [[nodiscard]] ui::Node view() override { return build(); }

    std::function<ui::Node()> build;
    morph::reactive::Signal<int> count;
    morph::reactive::TimerHandle timer;
};

}  // namespace

TEST_CASE("tui::Frontend: each input event is dispatched once: typing h, i reports \"hi\"", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    std::string seen;
    rig.source.pushEvents({key('h'), key('i')});
    rig.source.closeInput();
    auto const exitCode = rig.frontend().run([&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&seen] {
            return ui::column({.children = {ui::textInput({.onChange = [&seen](std::string text) { seen = std::move(text); }})}});
        };
        return app;
    });
    CHECK(exitCode == 0);
    CHECK(seen == "hi");
}

TEST_CASE("tui::Frontend: a timer-sent quit ends run with its exit code", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    auto const exitCode = rig.frontend().run([](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [] { return ui::text({.text = "idle"}); };
        app->timer = ctx.scheduler().after(10ms, [context = &ctx] { context->quit(7); });
        return app;
    });
    CHECK(exitCode == 7);
}

TEST_CASE("tui::Frontend: Ctrl+C quits with the interrupt exit code 130", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    rig.source.pushEvents({key('c', core::tui::Modifier::Ctrl)});
    auto const exitCode = rig.frontend().run([](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [] { return ui::text({.text = "idle"}); };
        app->timer = ctx.scheduler().after(2000ms, [context = &ctx] { context->quit(99); });  // only if Ctrl+C did nothing
        return app;
    });
    CHECK(exitCode == morph::tui::kInterruptExitCode);
    CHECK(morph::tui::kInterruptExitCode == 130);
}

TEST_CASE("tui::Frontend: Tab moves the focus through the tree", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    std::string clicks;
    rig.source.pushEvents({special(KeyCode::Tab), special(KeyCode::Enter), special(KeyCode::Tab), special(KeyCode::Enter)});
    rig.source.closeInput();
    static_cast<void>(rig.frontend().run([&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&clicks] {
            return ui::column({.children = {ui::button({.label = "A", .onClick = [&clicks] { clicks += 'A'; }}),
                                            ui::button({.label = "B", .onClick = [&clicks] { clicks += 'B'; }})}});
        };
        return app;
    }));
    CHECK(clicks == "BA");
}

TEST_CASE("tui::Frontend: Tab cycles inside an open dialog and never reaches the tree behind it", "[tui][frontend]") {
    Rig rig{{.width = 40, .height = 12}};
    std::string clicks;
    rig.source.pushEvents({special(KeyCode::Tab), special(KeyCode::Enter), special(KeyCode::Tab),
                           special(KeyCode::Enter), special(KeyCode::Tab), special(KeyCode::Enter)});
    rig.source.closeInput();
    static_cast<void>(rig.frontend().run([&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&clicks] {
            auto const pressed = [&clicks](char label) { return [&clicks, label] { clicks += label; }; };
            return ui::column({.children = {
                                   ui::button({.label = "A", .onClick = pressed('A')}),
                                   ui::button({.label = "B", .onClick = pressed('B')}),
                                   ui::dialog({.open = true,
                                               .title = "Q",
                                               .child = ui::column({.children = {ui::button({.label = "C", .onClick = pressed('C')}),
                                                                                 ui::button({.label = "D", .onClick = pressed('D')})}})}),
                               }});
        };
        return app;
    }));
    CHECK(clicks == "DCD");
}

TEST_CASE("tui::Frontend: a key whose handler writes state draws exactly one frame", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    int framesAtClick = -1;
    int framesAtQuit = -1;
    rig.source.pushEvents({special(KeyCode::Enter)});
    static_cast<void>(rig.frontend().run([&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        auto* const self = app.get();
        auto* const context = &ctx;
        app->build = [&rig, &framesAtClick, &framesAtQuit, self, context] {
            return ui::column({.children = {
                                   ui::text({.text = [self] { return std::to_string(self->count.get()); }}),
                                   ui::button({.label = "Bump",
                                               .onClick =
                                                   [&rig, &framesAtClick, &framesAtQuit, self, context] {
                                                       framesAtClick = rig.output->frames();
                                                       self->count.set(self->count.peek() + 1);
                                                       self->timer = context->scheduler().after(50ms, [&rig, &framesAtQuit, context] {
                                                           framesAtQuit = rig.output->frames();
                                                           context->quit(0);
                                                       });
                                                   }}),
                               }});
        };
        return app;
    }));
    CHECK(framesAtQuit - framesAtClick == 1);
}

TEST_CASE("tui::Frontend: a spinning Busy keeps frames coming", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    static_cast<void>(rig.frontend().run([](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [] { return ui::busy({.active = true, .label = "Working"}); };
        app->timer = ctx.scheduler().after(350ms, [context = &ctx] { context->quit(0); });
        return app;
    }));
    CHECK(rig.output->frames() >= 3);
}

TEST_CASE("tui::Frontend: the application is destroyed before the runtime", "[tui][frontend]") {
    morph::testing::StepExecutor unrelated;
    morph::testing::OwnerProbeRecorder const recorder{unrelated.coreExecutor()};
    Rig rig{{.width = 30, .height = 5}};
    rig.source.closeInput();
    static_cast<void>(rig.frontend().run([](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());  // owns a Signal on the runtime
        app->build = [] { return ui::text({.text = "idle"}); };
        return app;
    }));
    CHECK(recorder.count(morph::reactive::detail::site::kRuntimeOutlived) == 0);
}

TEST_CASE("tui::frontendOption names the frontend tui, asks whether stdin is a terminal, and makes one",
          "[tui][frontend]") {
    auto const option = morph::tui::frontendOption();
    CHECK(option.name == "tui");
    CHECK(option.usable() == core::platform::isTerminal(core::platform::standardInput()));
    auto const frontend = option.make();
    REQUIRE(frontend != nullptr);
    CHECK(frontend->name() == "tui");
}
```

Add `test_tui_frontend.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/tui --target morph_tui_tests`
Expected: FAIL — `'morph/tui/frontend.hpp' file not found`.

- [ ] **Step 3: Implement the session**

Create `src/tui/session.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <core/async/AsyncQueue.hpp>
#include <core/async/Task.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/runtime/TuiRuntime.hpp>
#include <memory>
#include <morph/core/io_loop.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/tui/backend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <morph/ui/frontend.hpp>
#include <string_view>

#include "tui/scheduler.hpp"

namespace morph::tui::detail {

/// One run of the TUI frontend: the AppContext the application is built with, its mounted view, the input pump,
/// one draw per loop turn, and the quit signal the pump races. Lives on the loop's (the calling) thread.
class Session final : public ui::AppContext {
public:
    Session(exec::IoLoop& loop, LoopExecutor& executor, ::core::tui::Screen& screen,
            ::core::tui::runtime::TuiRuntime& input);
    ~Session() override;
    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /// Builds the application, mounts its view and pumps input until quit() or the end of input. The application
    /// and its view are destroyed before this returns, so before the runtime.
    [[nodiscard]] int run(ui::ApplicationFactory const& factory);

    reactive::Runtime& runtime() override { return _runtime; }
    exec::IExecutor& executor() override { return *_executor; }
    ui::Scheduler& scheduler() override { return _scheduler; }
    exec::IoLoop* ioLoop() override { return _io; }
    void quit(int exitCode) override;
    [[nodiscard]] std::string_view frontendName() const override { return "tui"; }

private:
    [[nodiscard]] ::core::async::Task<int> serve();
    [[nodiscard]] ::core::async::Task<void> pump();
    [[nodiscard]] ::core::async::Task<void> awaitQuit();
    void handle(::core::tui::InputEvent const& event);
    void handleKey(::core::tui::KeyEvent const& key, ::core::tui::InputEvent const& event);
    void requestDraw();
    void draw();

    exec::IoLoop* _io;
    LoopExecutor* _executor;
    ::core::tui::Screen* _screen;
    ::core::tui::runtime::TuiRuntime* _input;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    reactive::Runtime _runtime;
    LoopScheduler _scheduler;
    Backend _backend;
    ::core::async::AsyncQueue<int> _quitSignal;
    std::atomic<int> _exitCode{0};
    std::atomic<bool> _quitting{false};
    bool _drawPosted = false;
    reactive::TimerHandle _animation;
};

}  // namespace morph::tui::detail
```

Create `src/tui/session.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tui/session.hpp"

#include <chrono>
#include <core/async/Cancellation.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/WhenAny.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <exception>
#include <morph/core/logger.hpp>
#include <morph/tui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace morph::tui::detail {

using ::core::tui::EventResult;

namespace {

/// How often a spinning Busy advances.
constexpr std::chrono::milliseconds kAnimationPeriod{100};

/// Ctrl+C. In raw mode it arrives as a key, not as SIGINT.
[[nodiscard]] bool isInterrupt(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::Ctrl &&
           (key.codepoint == U'c' || key.key == ::core::tui::keyCodeFromCodepoint(U'c'));
}

}  // namespace

Session::Session(exec::IoLoop& loop, LoopExecutor& executor, ::core::tui::Screen& screen,
                 ::core::tui::runtime::TuiRuntime& input)
    : _io{&loop},
      _executor{&executor},
      _screen{&screen},
      _input{&input},
      _runtime{executor, reactive::RuntimeOptions{.afterFlush = [this] { requestDraw(); }}},
      _scheduler{loop.loop(), executor},
      _backend{screen},
      _quitSignal{loop.loop(), ::core::async::AsyncQueueOptions{}} {}

Session::~Session() = default;

int Session::run(ui::ApplicationFactory const& factory) {
    _input->setInterruptHandler([this] { quit(kInterruptExitCode); });
    auto application = factory(*this);
    if (!application) {
        throw std::invalid_argument{"morph::tui::Frontend: the application factory returned no application"};
    }
    int exitCode = 0;
    {
        // Declared after the application, so the view's bindings die before the state they read.
        ui::Mounted const mounted{_runtime, _backend, application->view()};
        _backend.focusFirst();
        draw();
        exitCode = _input->blockOn(serve());
        _animation.cancel();
    }
    application.reset();
    return exitCode;
}

void Session::quit(int exitCode) {
    bool expected = false;
    if (_quitting.compare_exchange_strong(expected, true)) {
        _exitCode.store(exitCode);
        _quitSignal.close();
    }
}

::core::async::Task<int> Session::serve() {
    static_cast<void>(co_await ::core::async::whenAny(pump(), awaitQuit()));
    co_return _exitCode.load();
}

// Input that ends (a closed pipe, EOF on stdin) ends the run with exit code 0; a cancellation from whenAny,
// because quit() won the race, unwinds.
::core::async::Task<void> Session::pump() {
    for (;;) {
        std::optional<::core::tui::InputEvent> event;
        try {
            event = co_await _input->nextEvent();
        } catch (::core::async::OperationCancelled const&) {
            if (_input->inputClosed()) {
                co_return;
            }
            throw;
        }
        handle(*event);
    }
}

::core::async::Task<void> Session::awaitQuit() {
    static_cast<void>(co_await _quitSignal.pop());
}

// One dispatch per event, with the loop executor current so reactive writes in a handler are on the runtime's
// owner. A handler that throws is logged; the application keeps running.
void Session::handle(::core::tui::InputEvent const& event) {
    ::core::async::ExecutorScope const scope{_executor->coreExecutor()};
    try {
        if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event)) {
            handleKey(*key, event);
        } else {
            static_cast<void>(_screen->dispatchEvent(event));
        }
    } catch (std::exception const& failure) {
        ::morph::log::logError("[tui] an input handler threw: {}", failure.what());
    } catch (...) {
        ::morph::log::logError("[tui] an input handler threw an unknown exception");
    }
    requestDraw();
}

void Session::handleKey(::core::tui::KeyEvent const& key, ::core::tui::InputEvent const& event) {
    if (isInterrupt(key)) {
        quit(kInterruptExitCode);
        return;
    }
    auto const result = _screen->dispatchEvent(event);
    bool const tab = key.key == ::core::tui::KeyCode::Tab;
    bool const back = ::core::tui::hasModifier(key.modifiers, ::core::tui::Modifier::Shift);
    if (result == EventResult::FocusNext || (result == EventResult::Ignored && tab && !back)) {
        _backend.focusNext();
    } else if (result == EventResult::FocusPrev || (result == EventResult::Ignored && tab && back)) {
        _backend.focusPrev();
    }
}

// One draw per loop turn, however many flushes and events asked for one in the turn before.
void Session::requestDraw() {
    if (_drawPosted) {
        return;
    }
    _drawPosted = true;
    _executor->post([this, alive = std::weak_ptr<bool>{_alive}] {
        if (alive.expired()) {
            return;
        }
        _drawPosted = false;
        draw();
    });
}

void Session::draw() {
    _backend.fit();
    _screen->draw();
    if (_backend.animating()) {
        if (!_animation.active()) {
            _animation = _scheduler.every(kAnimationPeriod, [this] {
                _backend.advanceAnimation();
                requestDraw();
            });
        }
    } else {
        _animation.cancel();
    }
}

}  // namespace morph::tui::detail
```

- [ ] **Step 4: Implement the frontend**

Create `include/morph/tui/frontend.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/MouseTracking.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/runtime/InputSource.hpp>
#include <string_view>

#include "../ui/frontend.hpp"

/// @file
/// @brief `morph::tui::Frontend`: a morph application in a terminal.
///
/// Specified in `docs/spec/tui/frontend.md`.

namespace morph::tui {

/// @brief The exit code `Frontend::run` returns when Ctrl+C ended the run: the shell's 128 + SIGINT, so a script can
///        tell a user abort from a normal quit.
inline constexpr int kInterruptExitCode = 130;

/// @brief What a `tui::Frontend` runs on.
struct FrontendConfig {
    /// @brief The terminal to draw on; null means the process's terminal, which the frontend initialises, puts on
    ///        the alternate screen, and restores.
    ::core::tui::Terminal* terminal = nullptr;
    /// @brief Where input comes from; null means the terminal's own input. A test passes a scripted source.
    ::core::tui::runtime::InputSource* input = nullptr;
    /// @brief How much mouse input to ask the terminal for: `Drag` reports presses, releases and motion while a
    ///        button is held, which drag-and-drop needs; while it is on, a plain click-drag no longer selects
    ///        terminal text (Shift+drag still does in most terminals).
    ::core::tui::MouseTracking mouse = ::core::tui::MouseTracking::Drag;
};

/// @brief Runs a morph application in a terminal, on one thread.
///
/// `run` builds a caller-driven `exec::IoLoop`, a `LoopExecutor` on it (the reactive runtime's owner and every
/// bridge handler's callback executor), the runtime, a `core::tui::Screen` and a `TuiRuntime`, then calls the
/// factory, mounts its view and pumps input until `AppContext::quit()` or the end of input. Sockets, timers,
/// bridge callbacks and input all run on the thread that called `run`. Tab and Shift+Tab move the focus (inside
/// an open dialog only, while one is open); Ctrl+C quits; a resize re-fits the view; redraws are coalesced to one
/// per loop turn.
class Frontend final : public ui::Frontend {
public:
    /// @param config The terminal, the input and the mouse mode; the defaults use the process's terminal.
    explicit Frontend(FrontendConfig config = {});
    /// @brief Destroys the frontend; a `run` in progress must have returned.
    ~Frontend() override;
    Frontend(Frontend const&) = delete;
    Frontend& operator=(Frontend const&) = delete;
    Frontend(Frontend&&) = delete;
    Frontend& operator=(Frontend&&) = delete;

    /// @brief The name `--ui=` and `MORPH_UI` select this frontend by.
    /// @return `"tui"`.
    [[nodiscard]] std::string_view name() const override;

    /// @brief Runs the application on the calling thread until it quits or the input ends.
    /// @param factory Builds the application from the `ui::AppContext`; called once, after the runtime exists.
    /// @return The exit code given to `AppContext::quit()`; `kInterruptExitCode` (130) when Ctrl+C ended the run,
    ///         0 when the end of input did.
    /// @throws std::runtime_error when the terminal cannot be initialised; std::invalid_argument when the factory
    ///         returns no application.
    int run(ui::ApplicationFactory const& factory) override;

private:
    FrontendConfig _config;
};

/// @brief The TUI's entry for `ui::selectFrontend`.
/// @param config Handed to every `Frontend` the option makes.
/// @return An option named `"tui"`, usable when standard input is a terminal.
[[nodiscard]] ui::FrontendOption frontendOption(FrontendConfig config = {});

}  // namespace morph::tui
```

Create `src/tui/frontend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <core/platform/Types.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/runtime/TuiRuntime.hpp>
#include <memory>
#include <morph/core/io_loop.hpp>
#include <morph/tui/frontend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "tui/session.hpp"

namespace morph::tui {

Frontend::Frontend(FrontendConfig config) : _config{config} {}

Frontend::~Frontend() = default;

std::string_view Frontend::name() const {
    return "tui";
}

// Construction order is destruction order reversed, and it matters: the session (and with it the application)
// goes before the input runtime and the screen, those before the executor, and the executor before the loop.
// Everything is destroyed here, on the driving thread, outside a loop turn.
int Frontend::run(ui::ApplicationFactory const& factory) {
    exec::IoLoop ioLoop{exec::IoLoopDriver::Caller};
    LoopExecutor executor{ioLoop.loop()};
    std::unique_ptr<::core::tui::Terminal> owned;
    auto* terminal = _config.terminal;
    if (terminal == nullptr) {
        owned = std::make_unique<::core::tui::Terminal>();
        terminal = owned.get();
    }
    terminal->setMouseTracking(_config.mouse);
    if (auto const ready = terminal->initialize(); !ready) {
        throw std::runtime_error{"morph::tui::Frontend: the terminal did not initialise: " + ready.error()};
    }
    ::core::tui::Screen screen{*terminal, ::core::tui::ScreenConfig{.alternateScreen = owned != nullptr}};
    std::optional<::core::tui::runtime::TuiRuntime> input;
    if (_config.input != nullptr) {
        input.emplace(ioLoop.loop(), *_config.input);
    } else {
        input.emplace(ioLoop.loop(), *terminal);
    }
    detail::Session session{ioLoop, executor, screen, *input};
    return session.run(factory);
}

ui::FrontendOption frontendOption(FrontendConfig config) {
    return ui::FrontendOption{
        .name = "tui",
        .usable = [] { return ::core::platform::isTerminal(::core::platform::standardInput()); },
        .make = [config] { return std::unique_ptr<ui::Frontend>{std::make_unique<Frontend>(config)}; },
    };
}

}  // namespace morph::tui
```

Add `src/tui/frontend.cpp`, `src/tui/session.cpp` and `src/tui/session.hpp` to `add_library(morph_tui STATIC …)`,
and `include/morph/tui/frontend.hpp` to its `FILE_SET HEADERS`.

- [ ] **Step 5: Run the tests to verify they pass**

Run:

```bash
cmake --build build/tui --target morph_tui_tests morph_verify_interface_header_sets && ./build/tui/tests/tui/morph_tui_tests "[frontend]"
```
Expected: PASS, 9 test cases.

Mutation checks, one at a time, each restored before the next:
- In `Session::requestDraw`, delete the `if (_drawPosted) { return; }` guard. Expected: FAIL in "a key whose handler
  writes state draws exactly one frame" (2 frames: the event's draw and the flush's).
- In `Session::handleKey`, call `_screen->dispatchEvent(event)` twice. Expected: FAIL in "each input event is
  dispatched once" (`"hhii"`).
- In `Backend::focusNext` (`src/tui/backend.cpp`) call `_impl->context.screen->focusNext()` directly. Expected: FAIL
  in "Tab cycles inside an open dialog and never reaches the tree behind it".
- In `Session`, add a member `std::unique_ptr<ui::Application> _kept;` declared before `_runtime`, and in
  `Session::run` replace `application.reset();` with `_kept = std::move(application);`, so the application dies
  after the runtime. Expected: FAIL in "the application is destroyed before the runtime" (`kRuntimeOutlived` counted
  once: the application's `Signal` is still alive when the runtime goes).

- [ ] **Step 6: Commit**

```bash
git add include/morph/tui/frontend.hpp src/tui tests/tui CMakeLists.txt
git commit -m "wip(tui): tui::Frontend, its event pump, redraw coalescing and quit

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 18: The backend-conformance suite through a `TuiProbe`

**Files:**
- Modify: `tests/tui/CMakeLists.txt` — add `test_tui_conformance.cpp`
- Test: `tests/tui/test_tui_conformance.cpp`
- Fixes, if a case fails: the `src/tui/` file of the widget concerned

**Interfaces:**
- Consumes: `ui::testing::ConformanceProbe` (pure virtuals `backend`, `runtime`, `settle`, `textOf`, `visibleOf`,
  `enabledOf`, `childCount`, `childAt`, `click`, `type`, `drag`), `ui::testing::conformanceCases()`,
  `ui::testing::ConformanceCase{name, run}` (Part 2, `include/morph/ui/testing/backend_conformance.hpp`);
  `Backend` (Task 14); `LoopExecutor` (Task 5); `IoLoop{Caller}` (Task 1); `WidgetBase::of`, `probeText`,
  `userVisible`, `enabled`, `ContainerBase::of`, `children`, `asWidget` (Task 8). `visibleOf` is the widget's own
  flag (`userVisible()`, not `shown()`), as Part 2's `ConformanceProbe::visibleOf` defines it and
  `RecordingBackend` reports it.
- Produces: nothing public; the TUI backend's proof that it honours the same contract `RecordingBackend` defines.

- [ ] **Step 1: Write the test**

Create `tests/tui/test_tui_conformance.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The scripted cases every backend must pass (ui/testing/backend_conformance.hpp),
// run against the TUI backend. Clicks and typing go through Screen::dispatchEvent
// as key events, and drags as mouse events, so the cases exercise the input path
// a user's keys and mouse take.

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TestHelpers.hpp>
#include <cstddef>
#include <iterator>
#include <memory>
#include <morph/core/io_loop.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/tui/backend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "tui/widget.hpp"

namespace {

using morph::tui::detail::ContainerBase;
using morph::tui::detail::WidgetBase;

/// UTF-8 to code points; a byte that starts no valid sequence comes through on its own.
std::vector<char32_t> codepoints(std::string_view text) {
    std::vector<char32_t> out;
    auto byte = text.begin();
    while (byte != text.end()) {
        auto const lead = static_cast<unsigned char>(*byte);
        int const extra = lead >= 0xF0U ? 3 : lead >= 0xE0U ? 2 : lead >= 0xC0U ? 1 : 0;
        char32_t codepoint = extra == 0 ? lead : (lead & (0x3FU >> extra));
        ++byte;
        for (int i = 0; i < extra && byte != text.end(); ++i, ++byte) {
            codepoint = (codepoint << 6U) | (static_cast<unsigned char>(*byte) & 0x3FU);
        }
        out.push_back(codepoint);
    }
    return out;
}

/// The TUI backend on a headless 80x24 screen, its runtime owned by a loop executor on a caller-driven loop.
class TuiProbe final : public morph::ui::testing::ConformanceProbe {
public:
    TuiProbe() : _terminal{std::make_unique<core::tui::MockTerminalOutput>(80, 24)}, _screen{_terminal}, _backend{_screen} {}

    morph::ui::IViewBackend& backend() override { return _backend; }
    morph::reactive::Runtime& runtime() override { return _runtime; }

    void settle() override {
        static_cast<void>(_io.loop().runUntilIdle());
        _backend.fit();
        _screen.draw();
    }

    [[nodiscard]] std::string textOf(morph::ui::Widget const& widget) override { return WidgetBase::of(widget).probeText(); }
    [[nodiscard]] bool visibleOf(morph::ui::Widget const& widget) override { return WidgetBase::of(widget).userVisible(); }
    [[nodiscard]] bool enabledOf(morph::ui::Widget const& widget) override { return WidgetBase::of(widget).enabled(); }

    [[nodiscard]] std::size_t childCount(morph::ui::ContainerWidget const& container) override {
        return ContainerBase::of(container).children().size();
    }

    [[nodiscard]] morph::ui::Widget const* childAt(morph::ui::ContainerWidget const& container, std::size_t index) override {
        auto const children = ContainerBase::of(container).children();
        if (index >= children.size()) {
            return nullptr;
        }
        return &(*std::next(children.begin(), static_cast<std::ptrdiff_t>(index)))->asWidget();
    }

    void click(morph::ui::Widget& widget) override {
        settle();
        _screen.setFocus(&WidgetBase::of(widget).view());
        static_cast<void>(_screen.dispatchEvent(core::tui::test::specialKey(core::tui::KeyCode::Enter)));
        settle();
    }

    void type(morph::ui::Widget& widget, std::string_view text) override {
        settle();
        _screen.setFocus(&WidgetBase::of(widget).view());
        for (char32_t const codepoint : codepoints(text)) {
            static_cast<void>(_screen.dispatchEvent(core::tui::KeyEvent{
                .key = core::tui::keyCodeFromCodepoint(codepoint), .modifiers = core::tui::Modifier::None, .codepoint = codepoint}));
        }
        settle();
    }

    // The drag cases mount the source and the target as two roots, which fit() lays over the same viewport; side by
    // side, each has cells of its own under the pointer. settle() fits them back afterwards.
    void drag(morph::ui::Widget& source, morph::ui::Widget& target) override {
        settle();
        auto const area = _screen.viewportArea();
        int const half = area.width / 2;
        WidgetBase::of(source).view().setArea({.x = area.x, .y = area.y, .width = half, .height = area.height});
        WidgetBase::of(target).view().setArea(
            {.x = area.x + half, .y = area.y, .width = area.width - half, .height = area.height});
        _screen.draw();
        using Type = core::tui::MouseEvent::Type;
        auto const from = centre(WidgetBase::of(source).view().screenBounds());
        auto const destination = centre(WidgetBase::of(target).view().screenBounds());
        send(Type::Press, from);
        send(Type::Move, destination);
        send(Type::Release, destination);
        settle();
    }

private:
    static core::tui::Point centre(core::tui::Rect bounds) {
        return {.x = bounds.x + (bounds.width / 2), .y = bounds.y + (bounds.height / 2)};
    }

    void send(core::tui::MouseEvent::Type type, core::tui::Point cell) {
        static_cast<void>(_screen.dispatchEvent(core::tui::MouseEvent{.type = type, .button = 0, .x = cell.x + 1, .y = cell.y + 1}));
    }

    morph::exec::IoLoop _io{morph::exec::IoLoopDriver::Caller};
    morph::tui::LoopExecutor _executor{_io.loop()};
    morph::reactive::Runtime _runtime{_executor};
    core::tui::Terminal _terminal;
    core::tui::Screen _screen;
    morph::tui::Backend _backend;
};

}  // namespace

TEST_CASE("tui::Backend: the conformance suite is not empty", "[tui][conformance]") {
    CHECK_FALSE(morph::ui::testing::conformanceCases().empty());
}

TEST_CASE("tui::Backend passes the backend-conformance suite", "[tui][conformance]") {
    for (auto const& conformance : morph::ui::testing::conformanceCases()) {
        DYNAMIC_SECTION(conformance.name) {
            TuiProbe probe;
            auto const failure = conformance.run(probe);
            INFO(failure.value_or(std::string{}));
            CHECK_FALSE(failure.has_value());
        }
    }
}
```

Add `test_tui_conformance.cpp` to `tests/tui/CMakeLists.txt`.

- [ ] **Step 2: Run it**

Run: `cmake --build build/tui --target morph_tui_tests && ./build/tui/tests/tui/morph_tui_tests "[conformance]"`
Expected: PASS, one section per conformance case. A failing case is a TUI backend defect: fix the widget in
`src/tui/` (never the case, which `RecordingBackend` passes), add a widget-level test beside that widget's other
tests that pins the fix, and name the case and the fix in the hand-off.

- [ ] **Step 3: Prove the probe measures something**

Make `TuiProbe::textOf` return `std::string{}`. Expected: FAIL in every case that reads text ("a Text shows its
constant text" and "a bound Text updates once per batch and not for an equal write" among them). Restore. Then make
`TuiProbe::drag` send only the press. Expected: FAIL in "a drag onto an accepting target delivers the key" and "a
drop target without accepts takes every key". Restore. Then delete the two `setArea` calls in `TuiProbe::drag`.
Expected: FAIL in the same two cases (both roots cover the whole viewport, so both centres are one cell and the
press and the release land on the same root). Restore.

- [ ] **Step 4: Commit**

```bash
git add tests/tui src/tui
git commit -m "wip(tui): the backend-conformance suite against the TUI backend

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 19: Specs, maps and changelog

**Files:**
- Create: `docs/spec/tui/frontend.md`
- Modify: `docs/spec/README.md` — "Start here — specs by the question they answer"
- Modify: `docs/ARCHITECTURE.md` — "Namespace map" and "Header map"
- Modify: `docs/GETTING-STARTED.md` — §14 "Where to go next" table
- Modify: `README.md` — the namespace table (the row after `morph::qt`)
- Modify: `CHANGELOG.md` — `## [Unreleased]` → `### Added`, appended as its last entry

**Interfaces:**
- Consumes: the API exactly as Tasks 5–17 ship it. Where this text and the code disagree, fix the text.
- Produces: the authoritative spec later parts read before touching `morph::tui` (Part 6's smoke helper, the
  examples' composition roots).

- [ ] **Step 1: Write `docs/spec/tui/frontend.md`**

```markdown
# TUI frontend — design

Design spec for `morph::tui` (`include/morph/tui/`, sources in `src/tui/`, built
when `MORPH_BUILD_TUI=ON`): the terminal frontend that renders a `morph::ui`
view tree through core-cpp's `core::tui` and runs a whole application — its
UI, sockets, timers and bridge callbacks — on one thread.

## Contents

- [Packaging](#packaging)
- [One loop](#one-loop)
- [The loop executor](#the-loop-executor)
- [The frontend](#the-frontend)
- [Widgets](#widgets)
- [Layout](#layout)
- [Focus](#focus)
- [Drag-and-drop](#drag-and-drop)
- [Lifetimes](#lifetimes)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)

## Packaging

`morph_tui` (alias `morph::tui`) is a **static library**, the one optional
component compiled for a reason other than Qt's MOC: its widgets are
`core::tui::Component` subclasses, compiled once rather than in every consumer
(CONTRIBUTING, "Toolchain"). It links `morph` and `core::tui` PUBLIC.

| Public header | Contents |
|---|---|
| `tui/frontend.hpp` | `FrontendConfig`, `Frontend`, `frontendOption()` |
| `tui/backend.hpp` | `Backend`, the `ui::IViewBackend` over a `core::tui::Screen` |
| `tui/loop_executor.hpp` | `LoopExecutor` |

Everything else — the widgets, the layout solver, drag-and-drop, the
scheduler and the session — is private to `src/tui/`.

`MORPH_BUILD_TUI=ON` builds core-cpp with `CORE_CPP_WITH_TUI=ON` and
`CORE_CPP_WITH_IMAGES=OFF`, and provides libunicode 0.9.3 first (found, else
fetched; a fetch downloads `UCD.zip` from www.unicode.org). A core-cpp found
installed without `core::tui` stops the configure. The option is ignored under
Emscripten, where core-cpp builds no TUI. The `tui` install component ships
only when libunicode was found rather than fetched: a fetched libunicode is in
no export set, so core-cpp cannot install `core::tui` for `morph::tui` to link,
and the configure says so.

## One loop

`Frontend::run` builds `exec::IoLoop{IoLoopDriver::Caller}`
(`docs/spec/core/executor.md`, "The I/O loop") and a `TuiRuntime` on its
`loop()`. `TuiRuntime::blockOn` turns that loop on the calling thread for the
whole run, so the input pump, the reactive flushes, the redraws, timers,
sockets built on `ctx.ioLoop()` and every bridge callback delivered to
`ctx.executor()` share one thread and need no lock.

## The loop executor

`LoopExecutor` is a `morph::exec::IExecutor` over a `core::net::EventLoop`:

- `post(task)` queues `task` with `EventLoop::post`, from any thread; it runs
  in a later turn, inside `core::async::ExecutorScope{coreExecutor()}`, so
  `exec::runningOn(executor)` holds in it.
- A task still queued when the executor is destroyed is dropped: each queued
  task holds a weak token to the executor and checks it first.
- A task that throws is logged (`[tui] loop task threw: …`) and the loop goes
  on; one exception escaping a turn would end the run.

It is the reactive `Runtime`'s owner and every `BridgeHandler`'s callback
executor in a TUI application.

## The frontend

`Frontend(FrontendConfig)` — `terminal` (null: the process's terminal,
initialised, switched to the alternate screen and restored), `input` (null:
the terminal's input; a test passes a scripted source) and `mouse`
(`MouseTracking::Drag` by default). `name()` is `"tui"`; `frontendOption()`
names it `"tui"` and is usable when standard input is a terminal.

`run(factory)`, in order:

1. Builds the loop, the `LoopExecutor`, the terminal (asking for `mouse`
   tracking before `initialize()`), the `Screen` and the `TuiRuntime`.
2. Builds the session: the `reactive::Runtime` owned by the executor with
   `RuntimeOptions::afterFlush` requesting a draw, the `Scheduler`, the
   `Backend` and the quit signal.
3. Calls the factory with the session as `ui::AppContext`
   (`frontendName()` `"tui"`, `ioLoop()` the caller-driven loop), mounts
   `view()`, focuses the first focusable widget and draws.
4. Races the input pump against the quit signal (`core::async::whenAny`).
5. Destroys the mounted view, then the application, then the session, the
   input runtime, the screen, the executor and the loop — on the calling
   thread, outside a loop turn.

| Event | What happens |
|---|---|
| any input event | exactly one `Screen::dispatchEvent`; then a draw is requested |
| Tab / Shift+Tab, not consumed | focus moves forward / back (see [Focus](#focus)) |
| Ctrl+C | `quit(kInterruptExitCode)` (130, the shell's 128 + SIGINT); the key is not dispatched |
| a resize | the screen resizes its buffers; the next draw re-fits the root |
| the input ends | the run ends with exit code 0 |
| `AppContext::quit(code)` | the first call closes the quit signal; `run` returns `code` |

**One draw per loop turn.** A draw request posts one draw to the executor
unless one is already posted; a flush's `afterFlush` and an input event in the
same turn share it. A draw fits the root widgets to the viewport, centres open
dialogs and calls `Screen::draw()`.

**Timers.** `AppContext::scheduler()` arms `EventLoop` timers; each callback
runs in a loop turn with the executor current. A handle cancels on
destruction, may cancel its own timer from inside the callback, and may
outlive the scheduler. While any Busy widget is active the frontend advances
the spinner every 100 ms.

An exception thrown by an input handler is logged and the run goes on.

## Widgets

One private class per `ui` widget interface, each owning one
`core::tui::Component` view, registered with the backend while the widget
lives. Text is UTF-8 and measured in cells (`core::tui::stringWidth`).

| Widget | Renders | Keys and mouse |
|---|---|---|
| Text | one row per line; `TextRole` picks the theme style | — |
| Button | `[ label ]` | Enter, Space, click |
| Checkbox | `[x] label` / `[ ] label` | Enter, Space, click: flips and reports the new state |
| TextInput | a `core::tui::InputField` (masked for Password; multiline: Shift/Alt+Enter is a new line); the placeholder while empty | edits report `onChange` once per change of text; Enter reports `onSubmit`; Tab, Shift+Tab and Esc pass through |
| Select (Radio) | one row per option, `(•)` on the selection, `▶` on the highlight | Up/Down; Space or Enter selects |
| Select (Dropdown) | `[label ▾]`; open, a list overlay below it | Enter, Space or Down opens; in the list Enter chooses, Esc and Tab close |
| Menu | one list | Up/Down; Enter or Space activates |
| Column / Row / ForEach | children along the axis, `gap` apart | — |
| Grid | equal columns, row-major, spans | — |
| Spacer | nothing; Content-sized in a stack it stretches | — |
| Panel | a box with the title, children inset by 1 + padding; collapsible: `▾ title` / `▸ title` | Enter, Space, click toggle a collapsible panel |
| Scroll | the content, scrolled by whole children | the focused child is kept in view; the wheel moves one child |
| Slot (a Switch case, a Tabs page) | children stacked | — |
| Tabs | a bar, `[selected]` and ` other `, above the page slots | Left/Right, a click on a label |
| Dialog | an overlay box with the title, centred | Esc calls `onDismiss`; focus is trapped inside |
| Busy | a spinner (`\|/-\`) and the label while active | — |
| Table | a header row, then the rows in solved column widths; a two-cell gutter: `*` selected, `>` cursor | Up/Down/Home/End move the cursor; Space selects (Single) or toggles (Multiple); Enter selects (Single) and activates |
| DateTimeInput | `YYYY-MM-DD` or `YYYY-MM-DD HH:MM` in the display zone; `!` when the text does not parse | Up/Down step a day (Date) or a minute (DateTime); PageUp/PageDown a day; Enter commits, or clears when empty |
| Slider | `[====\|----] value` | Left/Right by step; Home/End to the ends |
| FilePicker | a path field prompted `Open: ` or `Save: ` | Enter reports a non-empty path |

**Controlled inputs.** `setText` with the text the field already holds does
nothing: a binding echoing what the user just typed must not move the cursor
or report a change. A Checkbox, a Select, a Tab and a Slider show a user's
choice at once and report it; a binding that disagrees sets them back.

**Tabs pages.** The children of a Tabs widget are the page slots the mount
creates, in the order their tabs were first shown, stacked under the bar. The
mount hides every page but the selected one through the page's own `visible`;
`setSelected` moves the bar's highlight only, because a child's position says
nothing about which tab it belongs to.

**Visibility** has two sources, combined: the widget's own `visible` and its
container's (a collapsed panel). A view counts as visible only while every
ancestor is, so a hidden container's children are neither drawn nor reachable
by Tab. The conformance probe's `visibleOf` reads the widget's own flag only.

**Dialog content** is mounted into the dialog before `setOpen(true)` and
destroyed before `setOpen(false)`. Opening focuses the first focusable widget
inside; closing hands the focus back also when the focused content was
destroyed first and left nothing focused.

## Layout

Containers place their children when the screen renders them; core::tui
renders a parent before its children, so the areas are current for that frame.
The solver (`src/tui/layout.hpp`) works in cells:

- **Along a stack**: Fixed takes its amount and Content its natural extent;
  Stretch items share what is left by weight (a weight below one counts as
  one), the cells a division leaves over going one each to the first Stretch
  items; an overflow is taken from the last item first, down to zero.
- **Across a stack**: Fixed takes its amount, at most the space; Content and
  Stretch fill it.
- **Grid**: equal-width columns; cells row-major; a span is clamped to the
  column count and a cell that does not fit its row starts the next; a row is
  as tall as its tallest cell (Fixed height wins); `gap` separates both
  columns and rows.
- **Padding** insets on every side, never below zero.

A hidden child takes no space. A root widget fills the viewport.

## Focus

Tab and Shift+Tab move the focus among focusable, visible widgets in tree
order, wrapping. `Screen::focusNext` walks only the tree, not overlays, so
while a Dialog is open the backend cycles among the focusable widgets inside
the innermost open dialog instead (its frame when there are none), and a press
outside that dialog is swallowed. Opening a dialog focuses its first focusable
widget; closing it hands the focus back to what had it, if that widget still
exists. A widget destroyed while focused clears the screen's focus first.

Shift+Tab reaches the application only from terminals speaking the Kitty
keyboard protocol: core-cpp does not decode the legacy `CSI Z`.

## Drag-and-drop

The frontend asks for `MouseTracking::Drag`. A press on a widget with a
`dragKey` arms a gesture; core::tui's pointer capture then sends every motion
and the release to that widget. The first motion one cell away starts the
drag: a label (`[first line of the source's text]`, or the key) follows two
columns right of the pointer. The drop target is the first widget, from the
one under the pointer (`Screen::componentAt`) up through its parents, other
than the source, whose `onDrop` is set and whose `accepts` is empty or holds
for the key; it is outlined with a double box. The release calls the target's
`onDrop(key)` after the label and outline are gone. A press and release with no
motion is a click. A source destroyed mid-gesture ends it and releases the
pointer; a target destroyed mid-gesture stops being one.

## Lifetimes

- The application and its mounted view die before the runtime; the view dies
  before the application, so bindings die before the state they read.
- Widgets die before the `Backend`, the `Backend` before the `Screen`.
- Overlays (a dialog's frame, a dropdown's list, the drag label and outline)
  are hidden by their owner's destructor.
- Everything is destroyed on the calling thread, outside a loop turn, where the
  loop's teardown is serialised with dispatch.

## Design decisions

- **One thread.** A terminal application has one natural owner thread; a
  caller-driven loop removes the cross-thread hop every reply and redraw would
  otherwise take.
- **Ctrl+C is taken before dispatch.** It always quits, with exit code 130
  (`kInterruptExitCode`, the shell's 128 + SIGINT, so a script can tell a user
  abort from a normal quit); a focused field never sees it.
- **Scroll by whole children.** core::tui clips a child at its parent's
  bounds but renders it from its own top row, so a child cannot be shown
  partly scrolled; scrolling moves whole children of the content stack.
- **Overlays for the outline and the label**, so the target's children cannot
  paint over them.

## Limitations

- No file dialog: FilePicker is a path field.
- The Dropdown's list is as wide as its longest option, not the field.
- Up/Down in a single-line TextInput browse core::tui's input history, which
  the frontend never fills.
```

- [ ] **Step 2: Maps, pointer and changelog**

`docs/spec/README.md`, "Start here": after the `**Reactive state and control**` group (and after the `morph::ui`
group, if Part 2 placed one after it), add:

```markdown
**Terminal frontend**
[`tui/frontend.md`](tui/frontend.md)
```

`docs/ARCHITECTURE.md`, "Namespace map": after the `morph::qt` row add

```markdown
| `morph::tui` | Terminal frontend (built only when `MORPH_BUILD_TUI=ON`) | `Frontend`, `FrontendConfig`, `frontendOption`, `Backend`, `LoopExecutor` |
```

and after the "### Qt integration headers (`include/morph/qt/`)" table add:

```markdown
### Terminal frontend headers (`include/morph/tui/`, with `MORPH_BUILD_TUI=ON`)

| Header | Responsibility |
|---|---|
| `tui/frontend.hpp` | `Frontend`, `FrontendConfig`, `frontendOption` — runs a `morph::ui` application in a terminal, on one caller-driven `IoLoop` |
| `tui/backend.hpp` | `Backend` — `ui::IViewBackend` over a `core::tui::Screen` |
| `tui/loop_executor.hpp` | `LoopExecutor` — a morph executor on a core-cpp event loop |
```

`docs/GETTING-STARTED.md`, §14 table: add as its last row

```markdown
| how do I run my UI in a terminal? | `docs/spec/tui/frontend.md` — the TUI frontend (`MORPH_BUILD_TUI`) |
```

`README.md`, namespace table: after the `morph::qt` row add

```markdown
| `morph::tui` | `tui/*.hpp` | `Frontend`, `Backend`, `LoopExecutor` — the terminal frontend (with `MORPH_BUILD_TUI=ON`) |
```

`CHANGELOG.md`, `## [Unreleased]` → `### Added`, append:

```markdown
- **`morph::tui`: a terminal frontend (`MORPH_BUILD_TUI`).** A compiled static
  library over core-cpp's `core::tui` — the first optional component compiled
  for a reason other than Qt's MOC. `tui::Frontend` runs a `morph::ui`
  application in a terminal on one caller-driven `IoLoop`, so sockets, timers,
  bridge callbacks and input share one thread: a `LoopExecutor` owns the
  reactive runtime, each input event is dispatched once, redraws are coalesced
  to one per loop turn, Tab and Shift+Tab move the focus (trapped inside an open
  dialog), Ctrl+C quits, and drag-and-drop runs on the mouse with pointer
  capture. `tui::Backend` implements every `ui::IViewBackend` widget;
  `tui::frontendOption()` is its entry for `ui::selectFrontend`.
  `MORPH_BUILD_TUI=ON` fetches libunicode 0.9.3 when none is installed; the
  `tui` install component ships when libunicode was found. Specified in
  `docs/spec/tui/frontend.md`.
```

- [ ] **Step 3: Build the docs with warnings as errors**

```bash
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
```

Expected: exits 0. Doxygen reads every `include/morph/**` header whether or not its option is on; a warning naming
`morph::tui` is a missing brief or `@param`/`@return` — fix the header. (Overrides inherit `ui::IViewBackend`'s and
`ui::Frontend`'s documentation.)

Then `markdownlint docs/spec/tui/frontend.md CHANGELOG.md` (or `pre-commit run --files …`): 119 columns, tables
exempt.

- [ ] **Step 4: Commit**

```bash
git add docs/spec/tui docs/spec/README.md docs/ARCHITECTURE.md docs/GETTING-STARTED.md README.md CHANGELOG.md
git commit -m "wip(tui): specify morph::tui

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 20: Verify and squash the `tui` group

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the full suite, with and without the option**

```bash
cmake --build build/tui && ctest --test-dir build/tui --output-on-failure
ctest --test-dir build/tui -L tui --output-on-failure
cmake -S . -B build/reactive -DMORPH_BUILD_TUI=OFF && cmake --build build/reactive && ctest --test-dir build/reactive
```

Expected: every test passes; `-L tui` runs the `morph_tui_tests` cases and reports a non-zero count (a label that
matches nothing exits 0 having run nothing — read the count); the OFF build configures without fetching libunicode
and builds nothing from `src/tui/`.

- [ ] **Step 2: Sanitizers** (Linux; on macOS an ASan configure of the same tree)

```bash
cmake --preset clang-asan -DMORPH_BUILD_TUI=ON && cmake --build --preset clang-asan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/tests/tui/morph_tui_tests asan
./build/clang-asan/tests/tui/morph_tui_tests
cmake --preset clang-tsan -DMORPH_BUILD_TUI=ON && cmake --build --preset clang-tsan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/tests/tui/morph_tui_tests tsan
./build/clang-tsan/tests/tui/morph_tui_tests
```

Expected: clean. ASan is the observer for Review Focus 3 and 5 (a destroyed focused widget, a destroyed drag
source) and for the overlay teardown in `DialogImpl`, `DropdownSelectImpl` and `DragController`; TSan for
`LoopExecutor`'s cross-thread post and `quit()` from a timer.

- [ ] **Step 3: clang-tidy over the changed lines** — CONTRIBUTING's recipe, with the clang-debug configure plus
  `-DMORPH_BUILD_TUI=ON` (so `src/tui/*.cpp` and `tests/tui/*.cpp` are in the compile database), against
  `origin/master...HEAD`, the file count printed and asserted non-zero. Expected: no findings. `src/tui/*.cpp` are
  main files, so their findings count even though `HeaderFilterRegex` excludes the private headers.

- [ ] **Step 4: Install/export** — `bash scripts/check_install_export.sh` passes, and Task 6, Step 3's TUI install
  still behaves as recorded there.

- [ ] **Step 5: Commit any fixes**, as `wip(tui): fixes from the sanitizer, tidy and install gates`; skip when there
  were none, and say so in the hand-off.

- [ ] **Step 6: Squash the group**

Follow the master plan's "Squashing a part" procedure with `key=tui` and this message:

```text
tui: morph::tui terminal frontend

morph::tui, a static library behind MORPH_BUILD_TUI over core-cpp's
core::tui. tui::Frontend runs a morph::ui application on one
caller-driven IoLoop: a LoopExecutor owns the reactive runtime, each
input event is dispatched once, redraws coalesce to one per loop turn,
Tab focus is trapped inside open dialogs, and Ctrl+C or quit() ends the
run. tui::Backend builds every ui widget as a core::tui component laid
out by a cell-based stack and grid solver, with drag-and-drop on pointer
capture. Install component tui; specified in docs/spec/tui/frontend.md.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure must show, in order, `docs: …`, `core: build against core-cpp 0.7`,
`reactive: …`, `ui: …`, `core: IoLoopDriver::Caller, the I/O loop on the calling thread`,
`tui: morph::tui terminal frontend`.
