// SPDX-License-Identifier: Apache-2.0

#pragma once

// Common helpers used across the morph test suite. Each helper here was being
// re-declared (often with slightly different names — SyncExec / SyncExecutor /
// InlineExec) inside ~15 individual test translation units; consolidating them
// keeps the test code consistent and lets the production API not have to expose
// test-only utilities.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace morph::testing {

/// @brief `IExecutor` that runs every posted task synchronously on the caller's thread.
///
/// Replaces ad-hoc `SyncExec` / `SyncExecutor` / `InlineExec` definitions that
/// were sprinkled across the test suite. Catch2 assertions executed inside the
/// posted lambda surface on the same thread as the test body, so failures keep
/// pointing at the right source location.
struct InlineExecutor : ::morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

/// @brief `IExecutor` that queues every posted task and runs them only when the
///        test explicitly asks, one at a time.
///
/// The public interleaving-test harness: any server
/// component built on `morph::exec::IExecutor` — `RemoteServer` included — can
/// be driven with fully deterministic, hand-stepped task ordering by
/// constructing it against a `StepExecutor` instead of a `ThreadPoolExecutor`.
/// `RemoteServer` posts every dispatch (both the top-level `handle()` post and
/// the per-model strand dispatch its internal `ModelStrands` performs) onto
/// whichever `IExecutor` it was constructed with, so controlling that one
/// executor is enough to control ordering end-to-end — no need to name
/// `morph::exec::detail::ModelStrands` or `morph::exec::detail::ModelId` to
/// get there. A test picks which of several pending tasks (e.g. two different
/// models' queued work) to run next via `runOne()`, observing `RemoteServer`'s
/// real per-model serialisation (a strand never posts its next task until the
/// previous one has run) while still controlling the order two *different*
/// models' work interleaves in.
///
/// Not thread-safe against concurrent `runOne()`/`runAll()` calls — intended
/// for single-threaded, single-stepping test code, mirroring `MainThreadExecutor`'s
/// "owning thread" contract but without its wall-clock `runFor()` drain.
class StepExecutor : public ::morph::exec::IExecutor {
public:
    /// @brief Enqueues @p task; does not run it.
    /// @param task Callable to run on a later `runOne()`/`runAll()` call.
    void post(std::function<void()> task) override {
        std::scoped_lock const lock{_mtx};
        _queue.push_back(std::move(task));
    }

    /// @brief Runs exactly one queued task, oldest first (FIFO).
    /// @return `true` if a task was run, `false` if the queue was empty.
    bool runOne() {
        std::function<void()> task;
        {
            std::scoped_lock const lock{_mtx};
            if (_queue.empty()) {
                return false;
            }
            task = std::move(_queue.front());
            _queue.pop_front();
        }
        task();
        return true;
    }

    /// @brief Runs every task currently queued, including ones a running task
    ///        itself posts (e.g. a strand re-arming for its next queued item).
    ///
    /// Bounded at @p maxSteps rather than looping until the queue is empty: a
    /// task that keeps re-posting more work to this executor (a bug in the
    /// code under test, or a harness misuse) would otherwise turn this into an
    /// undetectable infinite loop, hanging the test process with no assertion
    /// failure and no compile-time signal. Real drains in this suite finish
    /// within a handful of steps, so the default is generous headroom, not a
    /// tight bound callers need to reason about.
    /// @param maxSteps Upper bound on tasks run before giving up.
    /// @return Number of tasks run.
    std::size_t runAll(std::size_t maxSteps = 10'000) {
        std::size_t ran = 0;
        while (ran < maxSteps && runOne()) {
            ++ran;
        }
        if (ran == maxSteps) {
            throw std::runtime_error(
                "StepExecutor::runAll: exceeded maxSteps -- a task is likely re-posting "
                "indefinitely; use runOne() to step through and find it");
        }
        return ran;
    }

    /// @brief Number of tasks currently queued, awaiting a `runOne()`/`runAll()`.
    /// @return Queue depth.
    [[nodiscard]] std::size_t pending() const {
        std::scoped_lock const lock{_mtx};
        return _queue.size();
    }

private:
    mutable std::mutex _mtx;
    std::deque<std::function<void()>> _queue;
};

/// @brief An `IExecutor` that queues every posted task and runs them only
///        when explicitly stepped — never on its own thread.
///
/// Without this, strand-ordering bugs in code built over `IExecutor` (see
/// `test_remote_execute_ordering.cpp`'s use of it against `RemoteServer`, or
/// `examples/common/testkit/strand_interleaver.hpp`'s identical copy against
/// `ModelStrands` in the ladder's own tests) are probabilistic stress runs
/// instead of reproducible interleavings: a test controls exactly which
/// posted task runs next, rather than hoping real OS thread scheduling
/// happens to hit the race on a given run.
///
/// Single-threaded by construction: `post()` just appends to a deque under a
/// mutex (posts can legitimately arrive from other threads — e.g. code under
/// test posting a continuation from inside a running task — but every task
/// itself runs synchronously on whichever thread calls `step()`/
/// `runSchedule()`).
///
/// Duplicated from `examples/common/testkit/strand_interleaver.hpp` rather
/// than shared across the two build trees — that header has no reachable
/// include path from `tests/` (`morph_ladder_testkit`'s own include
/// directories do not cover the repo-root `tests/` directory, and
/// `test_support.hpp` is a private header for `morph_tests`' own
/// translation units, not an installed/exported one) — matching this
/// codebase's established convention for small, self-contained internal
/// details that would otherwise need new cross-module plumbing to share.
///
/// Unlike `ThreadPoolExecutor`/`ModelStrands`, a task's exception is not
/// caught and logged here: it propagates straight out of `step()`/
/// `runSchedule()` to the caller. That is deliberate — the caller is a test,
/// and the exception is often a `REQUIRE` failure the test needs to see
/// rather than have silently swallowed.
class DeterministicExecutor : public ::morph::exec::IExecutor {
public:
    void post(std::function<void()> task) override {
        std::lock_guard lock{_mtx};
        _queue.push_back(std::move(task));
    }

    /// @return The number of tasks currently queued and not yet run.
    [[nodiscard]] std::size_t pending() const {
        std::lock_guard lock{_mtx};
        return _queue.size();
    }

    /// @brief Runs the oldest-queued task. Throws if the queue is empty.
    void step() {
        std::function<void()> task;
        {
            std::lock_guard lock{_mtx};
            if (_queue.empty()) {
                throw std::runtime_error("DeterministicExecutor::step: queue is empty");
            }
            task = std::move(_queue.front());
            _queue.pop_front();
        }
        task();
    }

    /// @brief Runs tasks in the exact order given, by *current* queue
    ///        position at the moment each entry is consumed — so a task that
    ///        posts new work mid-schedule is reflected in later indices.
    ///        `order` must name every index that will exist by the time it's
    ///        reached; the simplest correct schedule is just `{0, 1, ..., n-1}`
    ///        run one at a time via repeated `step()` calls when a test only
    ///        wants strict FIFO — `runSchedule` exists for tests that
    ///        deliberately want a *non*-FIFO interleaving.
    /// @param order The queue indices to run, in caller-chosen order, each
    ///              read against the queue's *current* contents at the
    ///              moment it is consumed (see above).
    void runSchedule(const std::vector<std::size_t>& order) {
        for (auto index : order) {
            std::function<void()> task;
            {
                std::lock_guard lock{_mtx};
                if (index >= _queue.size()) {
                    throw std::runtime_error("DeterministicExecutor::runSchedule: index beyond current queue size");
                }
                task = std::move(_queue[index]);
                _queue.erase(_queue.begin() + static_cast<std::ptrdiff_t>(index));
            }
            task();
        }
    }

private:
    mutable std::mutex _mtx;
    std::deque<std::function<void()>> _queue;
};

// ── The wait primitives are for liveness. Never time across one ─────────────
//
// `waitUntil` below, and `WaitReply::await` further down, answer *"did this
// eventually happen?"*. They do not answer *"how long did this take?"*, and
// they cannot be made to: the `sleep_for` in the loop quantises every wait they
// return from up to a whole polling step, so an elapsed time taken across one
// of these calls reports the step and not the thing being waited for. On an
// idle machine that is the step almost exactly; on a busy one it is whatever
// the first predicate check happened to observe. Neither is the measurement.
//
// This is not hypothetical. `tests/bench/bench_dispatch_latency.cpp` timed
// `WaitReply::await()` around each of 2000 serial round trips and published a
// p50 of 5074 us, while the same processes reported ~176k executes/sec at
// concurrency 1 — a round trip of about 5.7 us. Three orders of magnitude, on
// a figure that had a CI gate on it. The fix there is to replace
// the waiter with a condition variable (`BlockingReply`); copy that
// shape if you need to time something.
//
// ── Audit of the call sites ─────────────────────────────────────────────────
//
// 433 poll sites were classified: 211 direct `waitUntil(` invocations across 43
// files, plus 222 `await()` invocations, which reach the same loop through
// `WaitReply::await`. Exactly one block in the tree takes an elapsed time or
// publishes a figure across one of them, and it is `bench_dispatch_latency.cpp`'s
// single `TEST_CASE` — three sites: the latency `await()`s, and the throughput
// drain, which sat *inside* the window it was divided into. Every other site is
// a liveness assertion, where the step costs suite latency and never
// correctness, and none of them were changed.
//
// Three sweeps produced that split, and re-running them is how to check the
// claim has not rotted: every call site's enclosing block scanned for a timing
// or figure construct; every duration subtraction anywhere in `tests/` and
// `examples/` (22 of them) checked for a poll inside the interval it measures;
// and every figure publisher — benchmark artifacts, Catch2 `BENCHMARK`, stdout
// — checked for a poll upstream of its number. The near misses are worth
// knowing, because each looks like a hit until the measured interval is read:
// `tests/net/test_socket_server.cpp`'s four elapsed-time `REQUIRE`s bracket
// `close()` and `~SocketServer()` directly, `tests/test_server_limits.cpp`'s
// Catch2 `BENCHMARK` busy-waits on `yield()` rather than sleeping, and
// `examples/common/testkit/test_fault_proxy.cpp`'s assertion across `pumpUntil`
// is a *lower* bound, which quantisation can only ever make easier to satisfy.
//
// ── What the step costs a suite run, measured ────────────────────────────────
//
// It is not free, and the bill reads the
// same two independent ways. Measured on an otherwise-quiet 12-core Linux box
// (clang 22.1.8, Release, load average 0.9-2.1), one binary instrumented to
// take the step from the environment so that the two arms differ in nothing
// else — not even code layout:
//
//     poll step | inside waitUntil | morph_tests wall | serial ctest
//     ----------+------------------+------------------+---------------
//        5 ms   | 17.27 s (n=3)    | 84.9 / 88.2 s    | 110.6 / 111.5 s
//        1 ms   |  7.53 s (n=3)    | 75.2 / 77.5 s    | 102.0 / 104.3 s
//
// The call count is identical at both steps — 2691 calls, 3443 sleeps at 5 ms
// against 7161 at 1 ms — so the 9.7 s between the first column's rows is the
// quantisation and nothing else. It is not more waiting; it is the same waiting
// rounded up. The wall-clock delta agrees with it, about 10 s on the binary and
// about 8 s under `ctest`, which is how CI runs the suite. Roughly 11% of a
// `morph_tests` run is this step.
//
// The step is nonetheless left at 5 ms, and lowering it is a separate change
// that needs its own evidence. 433 call sites inherit this default, and the
// first trials of the table above were taken while another build held this
// machine at load 13-16, where 1 ms came out *slower* by 43 s rather than
// faster by 10. A default that only wins on an idle machine is how a suite
// becomes flaky on a shared runner rather than faster on one, and the runner is
// the configuration that would have to be measured before changing it.

/// @brief Default polling budget for `waitUntil`. Picked to cover the slowest
///        TSan/Valgrind runs without making green tests visibly slow.
inline constexpr std::chrono::milliseconds kDefaultWaitBudget{2000};

/// @brief Default polling step for `waitUntil`.
inline constexpr std::chrono::milliseconds kDefaultWaitStep{5};

// ── Why these are two types and not two `milliseconds` ─────────────────────
//
// `waitUntil` used to take `(Pred, milliseconds budget = 2000ms,
// milliseconds step = 5ms)`: two adjacent, same-type, both-defaulted
// parameters whose values differ by 400x. Transposing them at a call site
// compiled silently and produced a 2000 ms poll inside a 5 ms budget -- one
// predicate check, then failure -- across the 433 poll sites the audit above
// counted. Nothing in the language or the lint stopped it; the transposed call
// was simply a different, wrong program.
//
// `WaitBudget` and `WaitStep` make that transposition a **compile error**. Both
// constructors are `explicit`, so neither a raw duration nor the other wrapper
// converts: the static assertions below `waitUntil` pin every case, and they
// fail the build of all 52 translation units that include this header if the
// hazard is ever reintroduced.
//
// Two escapes were available and both were rejected. A `NOLINT` would have
// removed the *warning* and left the hazard. Reordering the
// parameters so they are no longer adjacent would have removed the
// heuristic's view of them and left the hazard too -- that exact outcome was
// measured on `annotateExactBound`, where widening a parameter to
// `std::string_view` silenced `bugprone-easily-swappable-parameters` while the
// transposition still compiled.
//
// The raw `kDefaultWaitBudget` / `kDefaultWaitStep` constants stay
// `std::chrono::milliseconds` because call sites use them for things that are
// not `waitUntil` arguments -- a `condition_variable::wait_for` budget in
// `tests/test_backend_registration_surface.cpp`, a manual pump quantum in
// `tests/test_client_execute_deadline.cpp`. Wrapping those would have been
// churn with no hazard behind it.

/// @brief `waitUntil`'s overall polling budget: the longest it may wait before
///        giving up and returning `false`.
///
/// A distinct type from WaitStep so that passing the two in the wrong order is
/// a compile error rather than a 400x-wrong program. Construction is
/// deliberately explicit; write `WaitBudget{5s}` at the call site.
struct WaitBudget {
    /// @brief The budget itself.
    std::chrono::milliseconds value;

    /// @brief Wraps a duration as a budget.
    /// @param ms How long `waitUntil` may keep polling.
    explicit constexpr WaitBudget(std::chrono::milliseconds ms) noexcept : value{ms} {}
};

/// @brief `waitUntil`'s polling step: how long it sleeps between predicate
///        checks.
///
/// A distinct type from WaitBudget -- see that type, and the note above it, for
/// why. Construction is deliberately explicit; write `WaitStep{1ms}`.
struct WaitStep {
    /// @brief The step itself.
    std::chrono::milliseconds value;

    /// @brief Wraps a duration as a polling step.
    /// @param ms How long to sleep between predicate checks.
    explicit constexpr WaitStep(std::chrono::milliseconds ms) noexcept : value{ms} {}
};

/// @brief Polls @p pred until it returns `true` or @p budget elapses.
///
/// **Liveness only: never take an elapsed time across this call.** The step
/// quantises what it returns from — see the audit above `kDefaultWaitBudget`
/// for what that cost the one benchmark that tried.
///
/// Returns `true` if the predicate eventually became `true`, `false` if the
/// budget expired first. Sleeps for @p step between polls so we don't burn the
/// CPU. Sized for asynchronous test fixtures: most callers should just write
/// `REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));`
///
/// @tparam Pred Nullary predicate returning something contextually convertible
///              to `bool`.
/// @param pred The predicate to poll.
/// @param budget Longest time to keep polling before returning `false`.
/// @param step How long to sleep between two polls.
/// @return `true` if @p pred became `true` within @p budget.
template <typename Pred>
bool waitUntil(Pred pred, WaitBudget budget = WaitBudget{kDefaultWaitBudget},
               WaitStep step = WaitStep{kDefaultWaitStep}) {
    const auto deadline = std::chrono::steady_clock::now() + budget.value;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(step.value);
    }
    return true;
}

namespace detail {

// Satisfied when `waitUntil` is callable with `Args` -- the predicate type
// first, then whatever is passed after it. Plain comments rather than Doxygen
// because clang's `-Wdocumentation` does not accept `@tparam` on a concept.
template <typename... Args>
concept WaitUntilCallableWith = requires(Args... args) { waitUntil(args...); };

/// @brief A stand-in predicate type for the assertions below.
using ExampleWaitPred = bool (*)();

// The acceptance test for that separation, and the reason this header is the right
// place for it: these run in every translation unit that includes it, so the
// hazard cannot be reintroduced by a later edit without reddening the build.
//
// What must keep working -- essentially all 433 sites rely on the defaults:
static_assert(WaitUntilCallableWith<ExampleWaitPred>);
static_assert(WaitUntilCallableWith<ExampleWaitPred, WaitBudget>);
static_assert(WaitUntilCallableWith<ExampleWaitPred, WaitBudget, WaitStep>);

// What must not compile. The first is the transposition itself; the rest are
// the routes back to it, each of which would restore a silent 400x error.
static_assert(!WaitUntilCallableWith<ExampleWaitPred, WaitStep, WaitBudget>);
static_assert(!WaitUntilCallableWith<ExampleWaitPred, WaitStep>);
static_assert(!WaitUntilCallableWith<ExampleWaitPred, std::chrono::milliseconds>);
static_assert(!WaitUntilCallableWith<ExampleWaitPred, std::chrono::milliseconds, std::chrono::milliseconds>);
static_assert(!WaitUntilCallableWith<ExampleWaitPred, WaitBudget, WaitBudget>);
static_assert(!WaitUntilCallableWith<ExampleWaitPred, WaitStep, WaitStep>);

}  // namespace detail

/// @brief Collects a single `RemoteServer` reply and decodes it.
///
/// Designed to be passed as the reply callback to `RemoteServer::handle()`:
///
/// @code
/// morph::testing::WaitReply waiter;
/// server->handle(envelopeJson, std::ref(waiter));
/// REQUIRE(waiter.await());
/// REQUIRE(waiter.env.kind == "ok");
/// @endcode
///
/// The raw reply string is kept available for tests that need to inspect
/// malformed responses (when `env` may have failed to decode).
struct WaitReply {
    std::atomic<bool> ready{false};
    std::string raw;
    ::morph::wire::Envelope env;

    /// @brief Reply-callback entry point.
    void operator()(const std::string& msg) {
        raw = msg;
        try {
            env = ::morph::wire::decode(msg);
        } catch (...) {
            // Leave `env` default-initialised; tests inspecting `raw` can still assert.
        }
        ready.store(true);
    }

    /// @brief Blocks (polling) until the reply arrives or @p budget elapses.
    ///
    /// This is `waitUntil` under another name, and the "never time across one"
    /// note above `kDefaultWaitBudget` counts this function's call sites too.
    /// A round trip timed across `await()` reports the polling step.
    ///
    /// @param budget Longest time to wait for the reply.
    /// @return `true` if a reply arrived within the budget.
    bool await(std::chrono::milliseconds budget = kDefaultWaitBudget) {
        return waitUntil([this] { return ready.load(); }, WaitBudget{budget});
    }
};

}  // namespace morph::testing
