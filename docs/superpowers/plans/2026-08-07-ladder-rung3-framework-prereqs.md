# Rung 3 framework prerequisites — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the two framework gaps `examples/LADDER.md`'s "Framework
prerequisites" section names as blocking rung 3 (`polls`) — a client-side
execute deadline, and an async register-or-attach/attach path for
shared/keyed models — before any rung-3 app code is written.

**Architecture:** Both gaps are closed as small, surgical, opt-in additions
to existing chokepoints (`Bridge::executeVia` for the deadline;
`Bridge::attachHandler`/`ensureBound` plus `BridgeHandler::execute` for the
async attach path), each mirroring a pattern the framework already ships
elsewhere (`RemoteServer`'s server-side `TimeoutScheduler` for the deadline;
`IBackend::registerModelAsync`'s existing opt-in/fallback shape for the async
attach). Neither changes default behavior for any existing embedder — every
addition is either newly-constructed-only-when-configured or a `false`/`0`
default that falls straight back to today's exact code path.

**Tech Stack:** C++23, the morph core (`include/morph/core/`), Qt6 WebSocket
transport (`include/morph/qt/`, `src/qt/`), Catch2.

## Global Constraints

- C++23 throughout, matching every other file in `include/morph/core/`.
- **Zero default-behavior change.** Every embedder that has not explicitly
  opted in (a new config knob, defaulted off/0/disabled) must see byte-identical
  behavior after this plan as before it. This is not a style preference — it
  is the same guarantee `registerModelAsync`'s own doc comment states
  ("every backend that has not opted in ... is unaffected") and
  `RemoteServer::LimitPolicy::executeTimeout`'s existing opt-in shape
  (`0` = disabled) already sets as precedent in this exact codebase.
- **No new dependencies.** Both additions build on primitives the framework
  already has (`Completion`/`CompletionState`'s existing public constructor
  and idempotent `setValue`/`setException`; a relocated, unmodified copy of
  `RemoteServer`'s existing `TimeoutScheduler`).
- **Spec-first for public API.** Both additions are used by ordinary
  application code (any rung, not just polls) — `docs/spec/core/` gets a new
  section for each, in the same file and style as the feature it extends.
- **Every new public symbol needs complete Doxygen** (`@param`/`@return`/
  `@tparam` as applicable) — the Docs CI workflow (`WARN_AS_ERROR =
  FAIL_ON_WARNINGS`) enforces this for everything under `include/morph/`.

---

### Task 1: Client-side execute deadline

**Files:**
- Create: `include/morph/core/timeout_scheduler.hpp` (relocated from `remote.hpp`)
- Modify: `include/morph/core/remote.hpp` (drop the inline class, include the new header, update the qualified name)
- Modify: `include/morph/core/backend.hpp` (add `ClientTimeoutError`)
- Modify: `include/morph/core/bridge.hpp` (add `Bridge::setExecuteDeadline`, wire it into `executeVia`)
- Modify: `docs/spec/core/completion.md` (new section)
- Create: `tests/test_client_execute_deadline.cpp`

**Interfaces:**
- Produces: `morph::async::detail::TimeoutScheduler` (relocated, unmodified
  API: `Handle schedule(std::chrono::milliseconds, std::function<void()>)`,
  `void cancel(Handle)`) — every later rung's polling helper (starting with
  rung 3's own `GetEventsSince` client wrapper) builds on
  `Bridge::setExecuteDeadline` alone, not on this class directly.
- Produces: `morph::backend::ClientTimeoutError : std::runtime_error` —
  thrown to a pending `Completion` when `Bridge::setExecuteDeadline`'s
  duration elapses with no reply from any layer (distinct from
  `morph::backend::TimeoutError`, which means the *server* explicitly
  reported hitting `LimitPolicy::executeTimeout` — a `ClientTimeoutError`
  means nothing came back at all, dropped frame or hung server alike).
- Produces: `Bridge::setExecuteDeadline(std::chrono::milliseconds)` — opt-in,
  defaults to `std::chrono::milliseconds{0}` (disabled).

`RemoteServer`'s existing `TimeoutScheduler` (`include/morph/core/remote.hpp:66-167`,
currently `morph::backend::detail::TimeoutScheduler`) is a
self-contained, dependency-free, dedicated-background-thread
delay-then-fire-unless-cancelled primitive with no `Qt`/`IExecutor`
dependency of its own — exactly what a `Bridge`-owned client-side deadline
needs, since `Bridge` (`include/morph/core/bridge.hpp`) is transport- and
GUI-framework-agnostic. Relocate it unmodified into a new shared header so
both `RemoteServer` (server-side `executeTimeout`) and `Bridge` (this task's
client-side deadline) use the same class from one place, rather than
duplicating it.

- [ ] **Step 1: Relocate `TimeoutScheduler`**

Create `include/morph/core/timeout_scheduler.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "logger.hpp"

namespace morph::async::detail {

/// @brief Background scheduler that invokes a callback once after a delay, unless cancelled first.
///
/// Neither `Bridge` nor `RemoteServer` is bound to a specific `IExecutor`
/// with a delayed-post primitive, so a single dedicated thread per instance
/// tracks pending deadlines and fires callbacks when they elapse. Used by
/// `RemoteServer` to enforce `LimitPolicy::executeTimeout` (server-side —
/// see `docs/spec/core/backend.md`) and by `Bridge::setExecuteDeadline`
/// (client-side — see `docs/spec/core/completion.md`).
class TimeoutScheduler {
  public:
    /// @brief Opaque identifier for one scheduled callback.
    using Handle = std::uint64_t;

    /// @brief Starts the background thread.
    TimeoutScheduler() : _thread{[this] { run(); }} {}

    /// @brief Stops the background thread and joins it.
    ~TimeoutScheduler() {
        {
            std::scoped_lock const lock{_mtx};
            _stop = true;
        }
        _cv.notify_all();
        _thread.join();
    }

    TimeoutScheduler(const TimeoutScheduler&) = delete;
    TimeoutScheduler& operator=(const TimeoutScheduler&) = delete;
    TimeoutScheduler(TimeoutScheduler&&) = delete;
    TimeoutScheduler& operator=(TimeoutScheduler&&) = delete;

    /// @brief Schedules @p callback to run after @p delay on the scheduler's
    ///        background thread, unless cancelled first via `cancel()`.
    /// @param delay    Time to wait before firing.
    /// @param callback Invoked on the scheduler thread if not cancelled in time.
    ///                 Exceptions it throws are logged and swallowed.
    /// @return Handle usable with `cancel()`.
    Handle schedule(std::chrono::milliseconds delay, std::function<void()> callback) {
        auto const deadline = std::chrono::steady_clock::now() + delay;
        std::scoped_lock const lock{_mtx};
        Handle const handle = ++_nextHandle;
        auto iter = _entries.emplace(deadline, Entry{handle, std::move(callback)});
        _index[handle] = iter;
        _cv.notify_all();
        return handle;
    }

    /// @brief Cancels a previously scheduled callback immediately.
    ///
    /// If @p handle has not fired yet, its entry (and anything its callback
    /// captured) is erased right away — the caller does not have to wait for
    /// the original deadline for that memory to be released. A no-op if
    /// @p handle already fired or was already cancelled.
    /// @param handle Handle returned by a prior `schedule()` call.
    void cancel(Handle handle) {
        std::scoped_lock const lock{_mtx};
        auto found = _index.find(handle);
        if (found == _index.end()) {
            return;
        }
        _entries.erase(found->second);
        _index.erase(found);
    }

  private:
    struct Entry {
        Handle handle;
        std::function<void()> callback;
    };

    void run() {
        std::unique_lock lock{_mtx};
        while (!_stop) {
            if (_entries.empty()) {
                _cv.wait(lock);
                continue;
            }
            auto const nextDeadline = _entries.begin()->first;
            _cv.wait_until(lock, nextDeadline);
            if (_stop) {
                break;
            }
            auto now = std::chrono::steady_clock::now();
            while (!_entries.empty() && _entries.begin()->first <= now) {
                auto iter = _entries.begin();
                Entry entry = std::move(iter->second);
                _index.erase(entry.handle);
                _entries.erase(iter);
                lock.unlock();
                try {
                    entry.callback();
                } catch (const std::exception& exc) {
                    ::morph::log::logError("[timeout-scheduler] callback threw: " + std::string{exc.what()});
                } catch (...) {
                    ::morph::log::logError("[timeout-scheduler] callback threw unknown exception");
                }
                lock.lock();
                now = std::chrono::steady_clock::now();
            }
        }
    }

    std::mutex _mtx;
    std::condition_variable _cv;
    std::multimap<std::chrono::steady_clock::time_point, Entry> _entries;
    std::unordered_map<Handle, std::multimap<std::chrono::steady_clock::time_point, Entry>::iterator> _index;
    Handle _nextHandle{0};
    bool _stop{false};
    std::thread _thread;
};

}  // namespace morph::async::detail
```

This is a byte-for-byte copy of `remote.hpp:66-167`'s class body, only its
namespace changed (`morph::backend::detail` → `morph::async::detail`, since
its only two call sites — `RemoteServer` and, after this task,
`Bridge::executeVia` — both operate on `morph::async::CompletionState`-shaped
things, and `Completion`/`CompletionState` already live in `morph::async`).

- [ ] **Step 2: Update `remote.hpp` to use the relocated class**

In `include/morph/core/remote.hpp`:
1. Delete the inline `class TimeoutScheduler { ... };` definition (lines
   66-167 as of this plan's writing — confirm the exact range by searching
   for `class TimeoutScheduler` before deleting, since line numbers drift).
2. Add `#include "timeout_scheduler.hpp"` alongside the file's other
   `#include "..."` lines (near `#include "backend.hpp"`).
3. Every remaining use of `TimeoutScheduler` in this file
   (`_timeoutScheduler` member declaration and the 5 call sites found via
   `grep -n "TimeoutScheduler" include/morph/core/remote.hpp` before this
   change) is currently unqualified `detail::TimeoutScheduler`, resolved via
   this file's own `namespace morph::backend { namespace detail { ... } }`
   nesting. After the relocation it must be spelled
   `::morph::async::detail::TimeoutScheduler` at every one of those sites
   (an explicit, fully-qualified reference — do not add a `using` alias,
   which would silently shadow `morph::backend::detail` for anything else
   declared later in this file).

- [ ] **Step 3: Verify `RemoteServer`'s existing behavior is unchanged**

Run: `cmake --build build/clang-coverage --target morph_tests` then
`ctest --test-dir build/clang-coverage -R test_limit_policy`
Expected: identical pass count to a pre-change baseline (capture the
baseline first: `ctest --test-dir build/clang-coverage -R test_limit_policy`
before Step 1). This is a pure relocation — zero behavior change is the bar,
not "still passes."

- [ ] **Step 4: Add `ClientTimeoutError`**

In `include/morph/core/backend.hpp`, immediately after the existing
`TimeoutError` struct (currently lines 379-382 — confirm via
`grep -n "struct TimeoutError"` before editing):

```cpp
/// @brief Thrown to a pending `Completion` when `Bridge::setExecuteDeadline`'s
///        duration elapses before any reply arrives — a frame silently
///        dropped by `QtWebSocketServerConfig::messagesPerSecond`, or a
///        genuinely hung server, either way.
///
/// Distinct from `TimeoutError`: that type means the *server* explicitly
/// replied that it hit `LimitPolicy::executeTimeout` while the action was
/// still running. `ClientTimeoutError` means the client gave up waiting —
/// no reply of any kind arrived, so whether the server ever received the
/// request, is still processing it, or replied to a connection that had
/// already dropped is unknown. See `docs/spec/core/completion.md`.
struct ClientTimeoutError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    ClientTimeoutError() : std::runtime_error{"execute timed out waiting for any reply"} {}
};
```

- [ ] **Step 5: Wire the deadline into `Bridge`**

In `include/morph/core/bridge.hpp`:

1. Add `#include "timeout_scheduler.hpp"` to the file's includes.
2. Add a public method on `Bridge` (near `setDefaultSession`, which is the
   nearest existing "runtime-configurable knob" on this class — search
   `void setDefaultSession` to find it and place this beside it):

```cpp
/// @brief Sets (or disables) the client-side execute deadline.
///
/// Every `executeVia()` call after this point races the real reply against
/// @p deadline; whichever settles first wins (`CompletionState::setValue`/
/// `setException` are idempotent — see `completion.hpp`). If @p deadline
/// elapses first, the pending `Completion` fails with `ClientTimeoutError`;
/// the real reply, if it arrives later, is silently discarded exactly like
/// any other late write to an already-resolved `CompletionState`.
///
/// Disabled (`std::chrono::milliseconds{0}`, the default) reproduces
/// today's exact behavior: a dropped frame or a hung server leaves the
/// `Completion` pending forever, same as before this method existed.
///
/// @param deadline Maximum time to wait for any reply. `0` disables the
///                  deadline.
void setExecuteDeadline(std::chrono::milliseconds deadline) {
    std::scoped_lock const lock{_executeDeadlineMtx};
    _executeDeadline = deadline;
    if (_executeDeadline.count() > 0 && !_timeoutScheduler) {
        _timeoutScheduler = std::make_unique<::morph::async::detail::TimeoutScheduler>();
    }
}
```

3. Add the two private members it uses, next to `_sessionMtx`/`_defaultSession`
   (search for `_sessionMtx` to find the right neighborhood):

```cpp
mutable std::mutex _executeDeadlineMtx;
std::chrono::milliseconds _executeDeadline{0};
std::unique_ptr<::morph::async::detail::TimeoutScheduler> _timeoutScheduler;
```

4. In `executeVia` (search `Completion<typename ::morph::model::ActionTraits<Action>::Result> executeVia`
   to find it — as of this plan's writing at `bridge.hpp:691`), immediately
   after the `typedState`/`typed` pair is constructed and the `raw == 0U`
   fast-fail check has already returned (i.e., only real dispatches reach
   this point — a fast-failed "handler not bound" `Completion` needs no
   deadline, it's already resolved), read the deadline once and, if enabled,
   schedule it:

```cpp
        std::chrono::milliseconds deadline{0};
        {
            std::scoped_lock const lock{_executeDeadlineMtx};
            deadline = _executeDeadline;
        }
        std::optional<::morph::async::detail::TimeoutScheduler::Handle> deadlineHandle;
        if (deadline.count() > 0) {
            std::scoped_lock const lock{_executeDeadlineMtx};
            deadlineHandle = _timeoutScheduler->schedule(
                deadline, [typedState] { typedState->setException(std::make_exception_ptr(::morph::backend::ClientTimeoutError{})); });
        }
```

   (Place this block after the `raw == 0U` early-return, before
   `::morph::backend::detail::ActionCall call;` — the exact insertion point
   any implementer should confirm by reading the surrounding ~15 lines,
   since this plan quotes the method's shape from research, not a live
   diff.)

5. In the same method, the existing `anyCompletion.then(...).onError(...)`
   block (near the end of `executeVia`, already shown in this plan's
   research citations as ending with
   `.onError([typedState](const std::exception_ptr& err) { typedState->setException(err); });`)
   must cancel the scheduled deadline on **both** branches, before the
   `typedState->setValue`/`setException` call already there — add one line
   to each lambda's body:

```cpp
                if (deadlineHandle) {
                    std::scoped_lock const lock{_executeDeadlineMtx};
                    _timeoutScheduler->cancel(*deadlineHandle);
                }
```

   in the success lambda right before `typedState->setValue(std::move(*typedResult));`
   (inside the `try` block, after the `publishResult`/`onResult` work, so a
   thrown exception from that work still reaches the `catch` and the
   deadline is still cancelled — actually: cancel it as the *first* line of
   the lambda, before any of that other work, so a slow `onResult`/
   `publishResult` callback cannot race the deadline firing concurrently
   while this lambda is still running), and as the first line of the
   `.onError(...)` lambda, before `typedState->setException(err);`.
   `deadlineHandle`/`typedState` must both be captured by the lambdas that
   do not already capture them (the success lambda already captures
   `typedState`; add `deadlineHandle` — copied, it is a small
   `std::optional<uint64_t>` — to both lambdas' capture lists, plus `this`
   if not already captured, to reach `_timeoutScheduler`/`_executeDeadlineMtx`;
   the success lambda already captures `this`, so add `deadlineHandle` there;
   the error lambda currently captures only `typedState`, so add both `this`
   and `deadlineHandle`).

- [ ] **Step 6: Write the failing tests**

Create `tests/test_client_execute_deadline.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Coverage for the client-side execute deadline (examples/LADDER.md's
// "Framework prerequisites" #2): Bridge::setExecuteDeadline races the real
// reply against a client-owned timeout, so a frame silently dropped by
// QtWebSocketServerConfig::messagesPerSecond, or a genuinely hung server,
// no longer blocks the calling Completion forever.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <string>
#include <string_view>
#include <thread>

#include "test_support.hpp"

namespace {

struct DeadlineCount {
    int x = 0;
};

struct DeadlineModel {
    int execute(const DeadlineCount& a) { return a.x; }
};

// A backend whose execute() never resolves its Completion (until the test
// explicitly settles it), simulating a frame the server dropped -- no
// reply, ever, on this path -- or a hung server.
class NeverRepliesBackend : public morph::backend::detail::IBackend {
  public:
    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>) override {
        return morph::exec::detail::ModelId{1};
    }
    void deregisterModel(morph::exec::detail::ModelId) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId,
                                                              morph::backend::detail::ActionCall,
                                                              morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ++liveCompletions;
        return morph::async::Completion<std::shared_ptr<void>>{state, cbExec};
        // state is intentionally dropped here with no setValue/setException
        // ever called -- the Completion this returns never settles on its
        // own, matching a dropped frame or a server that never replies.
    }
    std::atomic<int> liveCompletions{0};
};

}  // namespace

template <>
struct morph::model::ActionTraits<DeadlineCount> {
    using Result = int;
    static constexpr std::string_view typeId() { return "Deadline_Count"; }
    static std::string toJson(const DeadlineCount& a) { return R"({"x":)" + std::to_string(a.x) + "}"; }
    static DeadlineCount fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& r) { return std::to_string(r); }
    static int resultFromJson(std::string_view s) { return std::stoi(std::string{s}); }
};
template <>
struct morph::model::ModelTraits<DeadlineModel> {
    static constexpr std::string_view typeId() { return "Deadline_Model"; }
};

TEST_CASE("Bridge::setExecuteDeadline(0) (the default) never fires -- a call that never replies "
          "stays pending, matching pre-existing behavior",
          "[core][bridge][client-deadline]") {
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge;
    bridge.setBackend(std::make_shared<NeverRepliesBackend>());
    morph::bridge::BridgeHandler<DeadlineModel> handler{bridge, &exec};

    bool resolved = false;
    handler.execute(DeadlineCount{.x = 1})
        .then([&resolved](int) { resolved = true; })
        .onError([&resolved](const std::exception_ptr&) { resolved = true; });
    exec.runFor(std::chrono::milliseconds{200});
    CHECK_FALSE(resolved);
}

TEST_CASE("Bridge::setExecuteDeadline fires ClientTimeoutError when no reply arrives in time",
          "[core][bridge][client-deadline]") {
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge;
    bridge.setBackend(std::make_shared<NeverRepliesBackend>());
    bridge.setExecuteDeadline(std::chrono::milliseconds{50});
    morph::bridge::BridgeHandler<DeadlineModel> handler{bridge, &exec};

    bool failed = false;
    bool threwClientTimeout = false;
    handler.execute(DeadlineCount{.x = 1}).onError([&](const std::exception_ptr& err) {
        failed = true;
        try {
            std::rethrow_exception(err);
        } catch (const morph::backend::ClientTimeoutError&) {
            threwClientTimeout = true;
        } catch (...) {
        }
    });
    // Poll rather than a single runFor(): the deadline fires on the
    // TimeoutScheduler's own background thread, which posts to `exec` --
    // give it real wall-clock slack, matching this codebase's other
    // cross-thread test patterns (see pumpUntil in examples/common/testkit).
    for (int i = 0; i < 50 && !failed; ++i) {
        exec.runFor(std::chrono::milliseconds{20});
    }
    REQUIRE(failed);
    CHECK(threwClientTimeout);
}

TEST_CASE("A deadline that is cancelled by a real, on-time reply does not also fire",
          "[core][bridge][client-deadline]") {
    // Uses the ordinary in-process LocalBackend, which always replies
    // quickly -- proves the cancellation path (Step 5's `.then`/`.onError`
    // cancel-before-settle lines), not just the firing path above.
    morph::exec::ThreadPoolExecutor workerPool{2};
    morph::exec::MainThreadExecutor guiExec;
    morph::bridge::Bridge bridge;
    bridge.setBackend(std::make_shared<morph::backend::LocalBackend>(workerPool));
    bridge.setExecuteDeadline(std::chrono::milliseconds{2000});  // generous; must not fire
    morph::bridge::BridgeHandler<DeadlineModel> handler{bridge, &guiExec};

    int result = -1;
    bool failed = false;
    handler.execute(DeadlineCount{.x = 7})
        .then([&result](int r) { result = r; })
        .onError([&failed](const std::exception_ptr&) { failed = true; });
    guiExec.runFor(std::chrono::milliseconds{500});
    CHECK(result == 7);
    CHECK_FALSE(failed);
    // If cancellation did not work, the 2000ms deadline is still pending on
    // the scheduler's background thread; the test process must not hang at
    // exit waiting for it -- Bridge's destructor and TimeoutScheduler's
    // destructor both join their threads unconditionally, so a leaked
    // pending entry would only delay (not hang) teardown. This assertion
    // exists to document that expectation, not to measure it directly.
}
```

- [ ] **Step 7: Confirm `test_client_execute_deadline.cpp` is picked up by the build**

Check `tests/CMakeLists.txt` (or wherever `morph_tests`' sources are
enumerated — likely a glob, matching every other file in `tests/`) actually
includes new files automatically; if it is an explicit list rather than a
glob, add the new file's path in the same style as its neighbors.

- [ ] **Step 8: Run to verify all three new tests fail without Step 4/5's code**

(A true red-first check only applies if you implement tests before code —
if Steps 4-5 are already done by this point, this step is a sanity
confirmation instead, matching this session's established pattern for
plan-supplied code where the feature predates the test by construction.)

- [ ] **Step 9: Run to verify all three tests pass**

Run: `cmake --build build/clang-coverage --target morph_tests && ctest --test-dir build/clang-coverage -R test_client_execute_deadline`
Expected: 3 test cases pass. Also re-run
`ctest --test-dir build/clang-coverage -R test_limit_policy` and the whole
`morph_tests`/`ladder` suites to confirm zero regressions.

- [ ] **Step 10: Update `docs/spec/core/completion.md`**

Add a new section (placement: wherever the file's existing structure best
fits a "how a `Completion` can fail" topic — read the file first and match
its heading style) documenting: `Bridge::setExecuteDeadline`'s opt-in shape
and default-disabled behavior; `ClientTimeoutError` vs. `TimeoutError`'s
distinction; the race-cancel-idempotent mechanics (a late real reply after
the deadline fired is silently discarded, not an error); and a
cross-reference to `docs/spec/core/backend.md`'s existing
`LimitPolicy::executeTimeout` section for the server-side counterpart.

- [ ] **Step 11: Commit**

```bash
git add include/morph/core/timeout_scheduler.hpp include/morph/core/remote.hpp \
        include/morph/core/backend.hpp include/morph/core/bridge.hpp \
        docs/spec/core/completion.md tests/test_client_execute_deadline.cpp \
        tests/CMakeLists.txt
git commit -m "core: add a client-side execute deadline (Bridge::setExecuteDeadline)"
```

---

### Task 2: Async register-or-attach and attach for shared/keyed models

**Files:**
- Modify: `include/morph/core/backend.hpp` (new `IBackend` virtuals)
- Modify: `include/morph/qt/qt_websocket_backend.hpp` and `src/qt/qt_websocket_backend.cpp` (real async implementation)
- Modify: `include/morph/core/bridge.hpp` (`Bridge::attachHandlerAsync`/`ensureBoundAsync`; `BridgeHandler::execute`'s `PayloadKeyed`/`ResultKeyed` branches)
- Modify: `docs/spec/core/shared_instances.md` (new section + API-reference rows)
- Modify: `tests/test_async_registration.cpp` (new test cases, same file — this is the established home for this exact class of coverage)

**Interfaces:**
- Consumes: Task 1's nothing directly (independent of the deadline work,
  but both must land before rung 3's app tasks — see this plan's
  "Execution order" note at the end).
- Produces: `IBackend::registerModelSharedAsync`/`attachModelAsync` — opt-in
  virtuals mirroring `registerModelAsync`'s exact shape (default returns
  `false`, invoking neither callback; a backend that opts in returns `true`
  and later invokes exactly one of `onRegistered`/`onError`).
  `QtWebSocketBackend` implements both for real, gated behind the same
  existing `QtWebSocketBackendConfig::asyncRegistrationEnabled` flag
  `registerModelAsync` already uses — no new config knob.
- Produces: no new public `BridgeHandler`/`Bridge` API surface — `execute()`'s
  existing signature and documented behavior ("A payload- or result-keyed
  action's attach/promote step never throws out of this call ... the
  failure is instead delivered through the returned Completion's
  `.onError(...)`") is unchanged; only *how* that promise is kept changes,
  transparently, when the backend offers an async path.

`IBackend::registerModelAsync`'s reply routing on `QtWebSocketBackend` is
already verb-agnostic: `onTextMessage`'s non-zero-`callId` branch
(`src/qt/qt_websocket_backend.cpp`, confirmed by reading it directly —
search `_pendingRegistrations.find(env.callId)`) matches *any* reply
carrying a matching `callId` against the same `_pendingRegistrations` map,
regardless of which wire verb (`register`, `registerShared`, `attach`)
produced the original request. `registerModelShared`'s wire form is a
`register` envelope with `primary`/`shared` fields added
(`docs/spec/core/shared_instances.md`, "Wire protocol changes" section);
`attach` is its own envelope kind but replies the same way (`ok` with a
`modelId`, or `err`). This means both new async methods are close to a
copy-paste of `registerModelAsync`'s existing body, substituting
`wire::makeRegisterShared`/`wire::makeAttach` for `wire::makeRegister` — no
new routing logic is needed on the reply-handling side at all.

`BridgeHandler<Model>::attach(key)` (the standalone public method, distinct
from `execute()`) is **out of scope** for this task: its own doc comment
already documents it as deliberately synchronous ("a caller that wants the
failure delivered asynchronously should attach via a payload-keyed action's
`execute()` instead") — this task makes that documented escape hatch real,
it does not change `attach()` itself. Rung 3's `OpenPoll{pollId}` is a
payload-keyed *action*, dispatched via `handler.execute(OpenPoll{pollId})`,
which is exactly the path this task covers.

- [ ] **Step 1: Add the two new `IBackend` virtuals**

In `include/morph/core/backend.hpp`, immediately after the existing
`registerModelAsync` declaration (confirm the exact line via
`grep -n "virtual bool registerModelAsync"`) and before
`registerModelShared`'s declaration:

```cpp
    /// @brief Optional non-blocking counterpart to `registerModelShared`.
    ///
    /// Same rationale and shape as `registerModelAsync` (see its doc comment
    /// immediately above): `registerModelShared`'s synchronous default
    /// implementations block the calling thread until a reply arrives, which
    /// aborts a WASM main thread the moment a shared/keyed handler makes its
    /// first attach. A backend that overrides this sends the request and
    /// returns `true` immediately, then invokes exactly one of
    /// @p onRegistered / @p onError once the reply arrives, on the backend's
    /// own thread (unless the backend is destroyed first, in which case
    /// neither fires).
    ///
    /// The default implementation offers no async path and returns `false`
    /// without calling either callback — the caller (`Bridge::ensureBoundAsync`)
    /// falls back to the synchronous `registerModelShared` in that case,
    /// matching every caller's behavior before this method existed.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param onRegistered Invoked with the assigned/attached `ModelId` on success.
    /// @param onError    Invoked with a diagnostic message on failure.
    /// @return `true` if this backend accepted the request and will invoke
    ///         exactly one callback later; `false` if it has no async path.
    virtual bool registerModelSharedAsync(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        ::morph::backend::detail::InstanceIdentity identity,
        std::function<void(::morph::exec::detail::ModelId)> onRegistered,
        std::function<void(const std::string&)> onError) {
        (void)typeId;
        (void)factory;
        (void)identity;
        (void)onRegistered;
        (void)onError;
        return false;
    }
```

And immediately after `attachModel`'s declaration:

```cpp
    /// @brief Optional non-blocking counterpart to `attachModel`.
    ///
    /// Same rationale and shape as `registerModelSharedAsync` immediately
    /// above (itself mirroring `registerModelAsync`) — see that doc comment
    /// for the full opt-in/fallback contract.
    /// @param typeId     String type-id of the model.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param current    Instance currently held, or `ModelId{0}` if none.
    /// @param onRegistered Invoked with the `ModelId` now attached to, on success.
    /// @param onError    Invoked with a diagnostic message on failure.
    /// @return `true` if this backend accepted the request and will invoke
    ///         exactly one callback later; `false` if it has no async path.
    virtual bool attachModelAsync(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        ::morph::backend::detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current,
        std::function<void(::morph::exec::detail::ModelId)> onRegistered,
        std::function<void(const std::string&)> onError) {
        (void)typeId;
        (void)factory;
        (void)identity;
        (void)current;
        (void)onRegistered;
        (void)onError;
        return false;
    }
```

Note `attachModelAsync` takes `current` but has no `factory`-driven
"deregister the old one first" step the way the synchronous
`IBackend::attachModel`'s *default* implementation does
(`backend.hpp:201-219`, acquire-before-release ordering) — `QtWebSocketBackend`'s
own synchronous `attachModel` already does not deregister `current` itself
either when `identity.primary` is non-empty (only the empty-primary
degrade-to-private-instance branch deregisters), so the async override
below follows that same existing division of responsibility, not a new one.

- [ ] **Step 2: Implement both in `QtWebSocketBackend`**

In `include/morph/qt/qt_websocket_backend.hpp`, add both declarations near
the existing `registerModelAsync` declaration (mirror its exact Doxygen
shape):

```cpp
    /// @brief Sends a shared (register-or-attach) `register` and, if async
    ///        registration is enabled, returns without blocking.
    /// @param typeId     String type-id of the model.
    /// @param factory    Ignored — model construction is delegated to the server.
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param onRegistered Invoked with the assigned `ModelId` on success.
    /// @param onError    Invoked with a diagnostic message on failure.
    /// @return `true` if `asyncRegistrationEnabled` is set (see
    ///         `QtWebSocketBackendConfig`) and the request was sent;
    ///         `false` otherwise, falling back to the synchronous
    ///         `registerModelShared`.
    bool registerModelSharedAsync(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        ::morph::backend::detail::InstanceIdentity identity,
        std::function<void(::morph::exec::detail::ModelId)> onRegistered,
        std::function<void(const std::string&)> onError) override;

    /// @brief Sends an `attach` and, if async registration is enabled,
    ///        returns without blocking.
    /// @param typeId     String type-id of the model.
    /// @param factory    Ignored — model construction is delegated to the server.
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param current    Instance currently held, or `ModelId{0}` if none.
    /// @param onRegistered Invoked with the `ModelId` now attached to, on success.
    /// @param onError    Invoked with a diagnostic message on failure.
    /// @return `true` if `asyncRegistrationEnabled` is set and the request
    ///         was sent; `false` otherwise, falling back to the synchronous
    ///         `attachModel`.
    bool attachModelAsync(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        ::morph::backend::detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current,
        std::function<void(::morph::exec::detail::ModelId)> onRegistered,
        std::function<void(const std::string&)> onError) override;
```

In `src/qt/qt_websocket_backend.cpp`, immediately after the existing
`registerModelAsync` definition (confirm exact location via
`grep -n "bool QtWebSocketBackend::registerModelAsync"`):

```cpp
bool QtWebSocketBackend::registerModelSharedAsync(
    const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/,
    ::morph::backend::detail::InstanceIdentity identity, std::function<void(::morph::exec::detail::ModelId)> onRegistered,
    std::function<void(const std::string&)> onError) {
    if (!_cfg.asyncRegistrationEnabled) {
        return false;
    }
    if (identity.primary.empty()) {
        // Degrades to the private (non-shared) path, exactly like the
        // synchronous registerModelShared above -- and that path already
        // has an async form: this class's existing registerModelAsync.
        return registerModelAsync(typeId, nullptr, identity.contextKey, std::move(onRegistered), std::move(onError));
    }
    if (!_connected) {
        onError("disconnected");
        return true;
    }
    uint64_t const callId = ++_nextCallId;
    {
        std::scoped_lock const lock{_pendingMtx};
        _pendingRegistrations[callId] = PendingRegistration{std::move(onRegistered), std::move(onError)};
    }
    auto env = ::morph::wire::makeRegisterShared(typeId, std::string{identity.primary}, std::string{identity.contextKey});
    env.callId = callId;
    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
    return true;
}

bool QtWebSocketBackend::attachModelAsync(
    const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/,
    ::morph::backend::detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current,
    std::function<void(::morph::exec::detail::ModelId)> onRegistered, std::function<void(const std::string&)> onError) {
    if (!_cfg.asyncRegistrationEnabled) {
        return false;
    }
    if (identity.primary.empty()) {
        // Mirrors the synchronous attachModel's empty-primary branch: release
        // the current instance (fire-and-forget, as deregisterModel already
        // is) and degrade to a private async registration.
        if (current.v != 0U) {
            deregisterModel(current);
        }
        return registerModelAsync(typeId, nullptr, identity.contextKey, std::move(onRegistered), std::move(onError));
    }
    if (!_connected) {
        onError("disconnected");
        return true;
    }
    uint64_t const callId = ++_nextCallId;
    {
        std::scoped_lock const lock{_pendingMtx};
        _pendingRegistrations[callId] = PendingRegistration{std::move(onRegistered), std::move(onError)};
    }
    auto env = ::morph::wire::makeAttach(typeId, std::string{identity.primary}, current.v, std::string{identity.contextKey});
    env.callId = callId;
    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
    return true;
}
```

Both reuse the exact same `_pendingRegistrations` map, `PendingRegistration`
struct, and reply-routing code `registerModelAsync` already has — confirm
by reading `onTextMessage`'s callId-routing branch (Step "research" already
verified this is verb-agnostic) that no changes are needed there.

- [ ] **Step 3: Add `Bridge`-side async attach/ensure-bound**

In `include/morph/core/bridge.hpp`, add `attachHandlerAsync`/`ensureBoundAsync`
immediately after the existing synchronous `attachHandler`/`ensureBound`
(same neighborhood, same access level — both are called from
`BridgeHandler::execute`, which is a friend or has appropriate access
already, matching how `attachHandler`/`ensureBound` are reached today):

```cpp
    /// @brief Async counterpart to `attachHandler`: prefers the backend's
    ///        `attachModelAsync` when available, invoking @p onDone once
    ///        attached (or failed) instead of blocking.
    ///
    /// Falls back to the synchronous `attachHandler` (and calls @p onDone
    /// immediately, from this thread) when the backend offers no async
    /// path — so a caller that always goes through this method behaves
    /// identically to calling `attachHandler` directly, on every backend
    /// that has not opted in to `attachModelAsync`.
    /// @tparam Model Concrete model type.
    /// @param binding Shared binding, as returned by `registerSharedHandler<Model>()`.
    /// @param primary Canonical string encoding of the primary key to attach to.
    /// @param onDone  Invoked with `nullptr` on success, or a non-null
    ///                `exception_ptr` on failure — always exactly once,
    ///                synchronously if the fallback path is taken.
    template <typename Model>
    void attachHandlerAsync(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary,
                             std::function<void(std::exception_ptr)> onDone) {
        std::scoped_lock const lock{_attachMtx};
        if (binding->primary == primary && binding->currentId.load() != 0U) {
            onDone(nullptr);
            return;
        }
        auto const previous = ::morph::exec::detail::ModelId{binding->currentId.load()};
        auto backend = loadBackend();
        auto primaryCopy = primary;
        std::weak_ptr<const void> const weakLiveness{_liveness};
        std::weak_ptr<detail::HandlerBinding> const weakBinding{binding};
        bool const started = backend->attachModelAsync(
            binding->typeId, binding->modelFactory, {.contextKey = primaryCopy, .primary = primaryCopy}, previous,
            [weakLiveness, weakBinding, primaryCopy, onDone](::morph::exec::detail::ModelId newId) {
                if (!weakLiveness.lock()) {
                    return;
                }
                auto strongBinding = weakBinding.lock();
                if (!strongBinding) {
                    return;
                }
                strongBinding->contextKey = primaryCopy;
                strongBinding->primary = primaryCopy;
                strongBinding->currentId.store(newId.v);
                onDone(nullptr);
            },
            [onDone](const std::string& message) { onDone(std::make_exception_ptr(std::runtime_error(message))); });
        if (!started) {
            try {
                auto newId = backend->attachModel(binding->typeId, binding->modelFactory,
                                                    {.contextKey = primary, .primary = primary}, previous);
                binding->contextKey = primary;
                binding->primary = std::move(primary);
                binding->currentId.store(newId.v);
                onDone(nullptr);
            } catch (...) {
                onDone(std::current_exception());
            }
        }
    }

    /// @brief Async counterpart to `ensureBound`. See `attachHandlerAsync`'s
    ///        doc comment for the fallback contract.
    /// @param binding Shared binding to bind.
    /// @param onDone  Invoked exactly once: `nullptr` on success, or a
    ///                non-null `exception_ptr` on failure.
    void ensureBoundAsync(const std::shared_ptr<detail::HandlerBinding>& binding,
                           std::function<void(std::exception_ptr)> onDone) {
        std::scoped_lock const lock{_attachMtx};
        if (binding->currentId.load() != 0U) {
            onDone(nullptr);
            return;
        }
        auto backend = loadBackend();
        std::weak_ptr<const void> const weakLiveness{_liveness};
        std::weak_ptr<detail::HandlerBinding> const weakBinding{binding};
        bool const started = backend->registerModelSharedAsync(
            binding->typeId, binding->modelFactory, {.contextKey = binding->contextKey, .primary = {}},
            [weakLiveness, weakBinding, onDone](::morph::exec::detail::ModelId newId) {
                if (!weakLiveness.lock()) {
                    return;
                }
                auto strongBinding = weakBinding.lock();
                if (!strongBinding) {
                    return;
                }
                strongBinding->currentId.store(newId.v);
                onDone(nullptr);
            },
            [onDone](const std::string& message) { onDone(std::make_exception_ptr(std::runtime_error(message))); });
        if (!started) {
            try {
                auto newId = backend->registerModelShared(binding->typeId, binding->modelFactory,
                                                            {.contextKey = binding->contextKey, .primary = {}});
                binding->currentId.store(newId.v);
                onDone(nullptr);
            } catch (...) {
                onDone(std::current_exception());
            }
        }
    }
```

Both hold `_attachMtx` only around the synchronous branch's own state
mutation and the async branch's *dispatch* (matching `attachHandler`'s
existing lock scope) — not around waiting for `onDone`, which for the async
path fires later, off this call stack entirely, on the backend's own
thread. This mirrors `registerHandlerImpl`'s existing doc comment
("the backend call must not run under `_mtx`") applied to `_attachMtx`
here: an async callback that reacquired `_attachMtx` from inside this
scope (which it does not — the scope ends when this method returns, well
before any async callback fires) would self-deadlock, so the shape above
(lock only around dispatch, not completion) is required, not incidental.

- [ ] **Step 4: Wire `BridgeHandler::execute` to use the async path**

In `include/morph/core/bridge.hpp`, `BridgeHandler<Model, Sharing>::execute`
(the method containing the `if constexpr (kShared && PayloadKeyed<Action>)`
and `if constexpr (kShared && ResultKeyed<Action>)` branches — confirm exact
line via `grep -n "if constexpr (kShared && ::morph::model::detail::PayloadKeyed"`).
Replace the `PayloadKeyed` branch's body:

```cpp
        if constexpr (kShared && ::morph::model::detail::PayloadKeyed<Action>) {
            auto state = std::make_shared<::morph::async::detail::CompletionState<R>>();
            ::morph::async::Completion<R> pending{state, _guiExec};
            auto* const bridgePtr = &_bridge;
            auto binding = _binding;
            auto key = ::morph::model::ActionKeyTraits<Action>::key(action);
            auto sharedAction = std::make_shared<Action>(std::move(action));
            bridgePtr->template attachHandlerAsync<Model>(
                binding, std::move(key), [bridgePtr, binding, sharedAction, state, guiExec = _guiExec](std::exception_ptr err) {
                    if (err) {
                        state->setException(err);
                        return;
                    }
                    bridgePtr->template executeVia<Model, Action>(binding, std::move(*sharedAction), guiExec)
                        .then([state](R r) { state->setValue(std::move(r)); })
                        .onError([state](std::exception_ptr e) { state->setException(e); });
                });
            return pending;
        }
```

This replaces the previous `try { attachHandler<Model>(...); } catch (...) { return failedCompletion<R>(...); }`
followed by the fallthrough `executeVia` call at the bottom of `execute()`
(the `else` branch) — the `PayloadKeyed` case now returns its own `pending`
`Completion` directly and never reaches the trailing
`return _bridge.template executeVia<Model, Action>(_binding, std::move(action), _guiExec);`
line, so that line's `if constexpr`/`else` structure must be adjusted:
confirm the surrounding `if constexpr (kShared && PayloadKeyed) { ... } if constexpr (kShared && ResultKeyed) { ... } else { ... }`
shape (three `if constexpr` chained, not `if/else if/else`, per the
existing code) still routes every other case (unkeyed actions, `NoSharing`
handlers) through the unchanged final `else` branch — this requires
`PayloadKeyed`'s branch to `return` unconditionally (as shown above) so
control never falls through to the trailing line for a payload-keyed
action, exactly matching today's control flow shape (today's `try`/`catch`
version also always exits the `if constexpr` block via its own `execute`
call after the block, but since `attachHandler` itself didn't return early,
double check whether today's structure already has an explicit early return
or relies on the outer `if constexpr`/`else` to skip the trailing call —
read the ~30 lines around this branch directly before editing, since the
plan's citation shows the shape but the implementer must confirm the exact
control-flow join point before rewriting it).

Apply the same treatment to the `ResultKeyed` branch, substituting
`ensureBoundAsync` for `attachHandlerAsync` and keeping the existing
`onResult` callback (the one that calls `assignHandlerPrimary`) wired the
same way it is today — attach it via `executeVia`'s existing `onResult`
parameter, unchanged, inside the `onDone` callback's non-error branch.

- [ ] **Step 5: Write the failing tests**

Append to `tests/test_async_registration.cpp` (this file already has a
`AsyncRegisterBackend` test-double pattern — read its existing ~362 lines
first and extend that same double with `registerModelSharedAsync`/
`attachModelAsync` overrides using the identical
`completeNext()`/`failNext()` deferred-completion shape the file already
uses for `registerModelAsync`, rather than inventing a second double). New
test cases, matching the file's existing `TEST_CASE` naming and structure:

- `"Bridge prefers attachModelAsync over the synchronous attachModel when the backend offers it"` — a keyed model, `AllowShared`, backend's async path deferred via the double's existing completion mechanism; assert the `Completion` returned by `execute(PayloadKeyedAction{...})` is still pending immediately after the call (proving no nested blocking occurred), then complete it and assert the result arrives.
- `"A backend with no async attach path falls back to the synchronous attachModel unchanged"` — a backend whose `attachModelAsync` override is absent (uses `IBackend`'s default, returning `false`) but whose synchronous `attachModel` works normally; assert `execute()` still succeeds exactly as before this task, proving zero regression for every backend that has not opted in.
- `"attachModelAsync's onError path surfaces through the returned Completion's onError, matching the synchronous path's documented contract"` — the double's `failNext()`; assert `.onError()` fires with the diagnostic message, never a synchronous throw out of `execute()` — the exact promise `execute()`'s own doc comment already makes.
- `"ensureBoundAsync mirrors the same three cases for a result-keyed (creating) action"` — repeat the three cases above for the `ResultKeyed`/`ensureBoundAsync` path using a `CreatePoll`-shaped test action (a minimal local double, not the real rung-3 `CreatePoll` — this file predates and is independent of rung 3).

- [ ] **Step 6: Run to verify all new tests fail without Steps 1-4's code, then pass with it**

Run: `cmake --build build/clang-coverage --target morph_tests && ctest --test-dir build/clang-coverage -R test_async_registration`
Expected: all cases (existing + new) pass. Also confirm
`tests/qt/test_qt_websocket.cpp` (the real `QtWebSocketBackend` suite) is
unaffected — run it too.

- [ ] **Step 7: Update `docs/spec/core/shared_instances.md`**

1. In the "API reference" table (search `## API reference`), add two rows
   documenting that `attach()`/keyed `execute()` now have an async path
   internally when the backend supports it — phrase this as an
   implementation detail visible only through *not blocking on WASM*, since
   `execute()`'s public signature and contract are unchanged (see this
   task's Interfaces section above).
2. Add a new subsection after "Wire protocol changes" (search
   `## Wire protocol changes`), titled something like "Async register-or-attach
   and attach", documenting: the opt-in shape (mirrors `registerModelAsync`,
   gated by the same `QtWebSocketBackendConfig::asyncRegistrationEnabled`),
   why `attach()` itself (the standalone method) remains synchronous by
   design while `execute()`'s keyed paths gained the async option, and a
   cross-reference to `examples/LADDER.md`'s "Framework prerequisites" #1
   as the motivating rung-3 WASM scenario this closes.

- [ ] **Step 8: Commit**

```bash
git add include/morph/core/backend.hpp include/morph/qt/qt_websocket_backend.hpp \
        src/qt/qt_websocket_backend.cpp include/morph/core/bridge.hpp \
        docs/spec/core/shared_instances.md tests/test_async_registration.cpp
git commit -m "core: add an async register-or-attach/attach path for shared/keyed models"
```

---

## Self-Review

**Spec coverage against `examples/LADDER.md`'s "Framework prerequisites"
section:** items 1 and 2 (async shared/keyed attach; client-side execute
deadline) are this plan's whole scope — both fully covered. Items 3
(injectable time source) and 4 (fault-injection wire proxy, deterministic
strand interleaver) were already closed in rung 0's own work (confirmed via
`git log --oneline` showing "ladder: add the fault-injection wire proxy"
and "ladder: add the deterministic strand interleaver" as existing
commits on this branch, predating this plan) — not reopened here.

**Placeholder scan:** none — every step above contains real, complete code
(not "TBD"/"add appropriate handling"), matching this plan's own "No
Placeholders" obligation. Where a step asks the implementer to confirm an
exact line number or control-flow join point before editing (Task 2, Step
4's note on the `if constexpr` structure), that is a verification
instruction, not a placeholder — the target *behavior* is fully specified
even where the exact line range is not, because this plan's own research
read the file's current shape but a live diff may have moved by
implementation time.

**Type/signature consistency check:** `ClientTimeoutError`'s shape matches
`TimeoutError`/`DisconnectedError`'s existing pattern
(`std::runtime_error` subclass, no members, canned message) exactly.
`registerModelSharedAsync`/`attachModelAsync`'s signatures mirror
`registerModelAsync`'s parameter order and callback shapes exactly
(`onRegistered` before `onError`, both `std::function`, both invoked
exactly once). `attachHandlerAsync`/`ensureBoundAsync`'s `onDone`
convention (`nullptr` = success, non-null `exception_ptr` = failure) is
used identically at every call site across Task 2, Steps 3-4.

**Judgment calls this plan made that the original LADDER.md prerequisite
text did not fully specify:**

1. **`TimeoutScheduler` relocates to `morph::async::detail`, not a new
   `morph::core` or `morph::backend`-adjacent namespace.** Chosen because
   both of its only two call sites (server-side `RemoteServer`, client-side
   `Bridge::executeVia`) operate on `CompletionState`-shaped things already
   in `morph::async`, and `Completion`/`CompletionState` are the class's
   only real conceptual neighbor (a delay-then-set-exception primitive, not
   a general-purpose scheduler).
2. **`ClientTimeoutError` is a distinct type from `TimeoutError`, not a
   reused one.** A caller that wants to distinguish "the server confirmed
   it hit its own timeout" from "nothing came back at all" needs this
   distinction — conflating them would silently lose that information for
   every future rung's retry/backoff logic.
3. **`attach()` (the standalone `BridgeHandler` method) is explicitly left
   synchronous.** Its own existing doc comment already documents this as
   the deliberate design (a caller wanting async should use a payload-keyed
   `execute()` instead) — this plan makes that documented escape hatch
   real rather than second-guessing the existing design.
4. **`registerModelSharedAsync`/`attachModelAsync`'s empty-`primary`
   branches degrade to the existing `registerModelAsync`, not a new
   private-instance async path.** Mirrors the synchronous
   `registerModelShared`/`attachModel`'s own existing degrade-to-private
   behavior exactly (`backend.hpp`'s doc comments on both), so this task
   adds no new private-instance semantics, only an async form of behavior
   that already exists.

## Execution order

Both tasks are independent of each other (neither's code touches the
other's files) and may be implemented in either order; this plan lists
Task 1 first only because it is the smaller, more self-contained of the
two. **Both must be complete, reviewed, and merged into `application-ladder`
before rung 3 (`polls`)'s own implementation plan begins** — `docs/superpowers/plans/2026-08-07-ladder-rung3-polls.md`'s
GUI/WASM-client tasks assume `Bridge::setExecuteDeadline` and the async
attach path both already exist and are tested.

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-08-07-ladder-rung3-framework-prereqs.md`.
Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task,
review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using
`executing-plans`, batch execution with checkpoints.

**If Subagent-Driven chosen:**
- **REQUIRED SUB-SKILL:** Use `superpowers:subagent-driven-development`
- Fresh subagent per task + two-stage review

**If Inline Execution chosen:**
- **REQUIRED SUB-SKILL:** Use `superpowers:executing-plans`
- Batch execution with checkpoints for review
