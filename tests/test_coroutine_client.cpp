// SPDX-License-Identifier: Apache-2.0
//
// The client side of docs/spec/core/coroutines.md: a coroutine awaiting a
// morph::async::Completion<T>, started with morph::async::spawn.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <functional>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/executor.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "compile_checks/completion_await_rvalue_only.hpp"
#include "test_support.hpp"

namespace {

using morph::async::Completion;

/// Pumps @p exec in bounded steps until @p pred holds or the budget is spent.
template <typename Pred>
[[nodiscard]] bool pumpUntil(morph::exec::MainThreadExecutor& exec, Pred pred) {
    auto const deadline = std::chrono::steady_clock::now() + morph::testing::kDefaultWaitBudget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        exec.runFor(morph::testing::kDefaultWaitStep);
    }
    return true;
}

struct Observed {
    std::optional<int> value;
    std::string error;
    bool cancelled = false;
    std::thread::id resumedOn;
    /// Set immediately before the `co_await`, in the same step that suspends.
    std::atomic<bool> reachedAwait{false};
    std::atomic<bool> finished{false};
};

core::async::Task<void> awaitInto(Completion<int> completion, std::shared_ptr<Observed> seen) {
    try {
        seen->reachedAwait = true;
        int const value = co_await std::move(completion);
        seen->value = value;
    } catch (const core::async::OperationCancelled&) {
        seen->cancelled = true;
    } catch (const std::exception& exc) {
        seen->error = exc.what();
    }
    seen->resumedOn = std::this_thread::get_id();
    seen->finished = true;
}

}  // namespace

TEST_CASE("co_await on a Completion yields the settled value", "[coroutine][client]") {
    morph::exec::MainThreadExecutor exec;
    auto [completion, promise] = Completion<int>::makeSettleable(&exec);
    auto seen = std::make_shared<Observed>();

    morph::async::spawn(exec, awaitInto(std::move(completion), seen));
    promise.resolve(42);

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->value == 42);
    REQUIRE(seen->error.empty());
}

TEST_CASE("co_await on a rejected Completion rethrows its exception", "[coroutine][client]") {
    morph::exec::MainThreadExecutor exec;
    auto [completion, promise] = Completion<int>::makeSettleable(&exec);
    auto seen = std::make_shared<Observed>();

    morph::async::spawn(exec, awaitInto(std::move(completion), seen));
    promise.reject(std::make_exception_ptr(std::runtime_error{"rejected on purpose"}));

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE_FALSE(seen->value.has_value());
    REQUIRE(seen->error == "rejected on purpose");
}

TEST_CASE("co_await resumes on the executor, not on the thread that settled the Completion", "[coroutine][client]") {
    morph::exec::MainThreadExecutor exec;
    auto [completion, promise] = Completion<int>::makeSettleable(&exec);
    auto seen = std::make_shared<Observed>();

    morph::async::spawn(exec, awaitInto(std::move(completion), seen));
    // Nothing runs until the executor is pumped: spawn posts the first step too.
    REQUIRE_FALSE(seen->finished.load());

    std::thread settler{[&promise] { promise.resolve(7); }};
    settler.join();

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->value == 7);
    REQUIRE(seen->resumedOn == std::this_thread::get_id());
}

TEST_CASE("a stop request withdraws the await and releases the coroutine's captures", "[coroutine][client]") {
    morph::exec::MainThreadExecutor exec;
    auto [completion, promise] = Completion<int>::makeSettleable(&exec);
    auto seen = std::make_shared<Observed>();
    auto capture = std::make_shared<int>(0);
    std::weak_ptr<int> const captureObserver = capture;

    auto task = [](Completion<int> pending, std::shared_ptr<Observed> observed,
                   std::shared_ptr<int> held) -> core::async::Task<void> {
        static_cast<void>(held);
        co_await awaitInto(std::move(pending), std::move(observed));
    }(std::move(completion), seen, std::move(capture));
    core::async::StopSource stop;
    task.handle().promise().setStopToken(stop.get_token());

    morph::async::spawn(exec, std::move(task));
    // The step that sets the flag is the one that suspends, and runs to its
    // end before `runFor` returns: once seen, the coroutine is suspended on a
    // completion that nothing settles.
    REQUIRE(pumpUntil(exec, [&] { return seen->reachedAwait.load(); }));
    REQUIRE_FALSE(seen->finished.load());
    REQUIRE_FALSE(captureObserver.expired());

    std::thread stopper{[&stop] { stop.request_stop(); }};
    stopper.join();

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->cancelled);
    REQUIRE(seen->resumedOn == std::this_thread::get_id());
    // The frame has finished and been freed, and the completion's handlers
    // hold none of it.
    REQUIRE(pumpUntil(exec, [&] { return captureObserver.expired(); }));

    // A settlement after the stop reaches nothing.
    promise.resolve(1);
    exec.runFor(std::chrono::milliseconds{20});
    REQUIRE_FALSE(seen->value.has_value());
}

TEST_CASE("co_await on an empty Completion throws std::logic_error without suspending", "[coroutine][client]") {
    morph::exec::MainThreadExecutor exec;
    auto seen = std::make_shared<Observed>();

    morph::async::spawn(exec, awaitInto(Completion<int>{}, seen));

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE_FALSE(seen->error.empty());
}

TEST_CASE("co_await on a Completion with no callback executor throws std::logic_error without suspending",
          "[coroutine][client]") {
    morph::exec::MainThreadExecutor exec;
    auto seen = std::make_shared<Observed>();
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();

    morph::async::spawn(exec, awaitInto(Completion<int>{state, nullptr}, seen));

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->error.contains("no callback executor"));
}

TEST_CASE("co_await under a stop already requested does not suspend", "[coroutine][client]") {
    morph::exec::MainThreadExecutor exec;
    auto [completion, promise] = Completion<int>::makeSettleable(&exec);
    auto seen = std::make_shared<Observed>();

    auto task = awaitInto(std::move(completion), seen);
    // Not const: request_stop() is a non-const member in the standard.
    // NOLINTNEXTLINE(misc-const-correctness)
    core::async::StopSource stop;
    stop.request_stop();
    task.handle().promise().setStopToken(stop.get_token());
    morph::async::spawn(exec, std::move(task));

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->cancelled);
    promise.resolve(1);
    exec.runFor(std::chrono::milliseconds{20});
    REQUIRE_FALSE(seen->value.has_value());
}

TEST_CASE("a rejection after a stop withdrew the await reaches nothing", "[coroutine][client]") {
    morph::exec::MainThreadExecutor exec;
    auto [completion, promise] = Completion<int>::makeSettleable(&exec);
    auto seen = std::make_shared<Observed>();

    auto task = awaitInto(std::move(completion), seen);
    // NOLINTNEXTLINE(misc-const-correctness): request_stop() is non-const
    core::async::StopSource stop;
    task.handle().promise().setStopToken(stop.get_token());
    morph::async::spawn(exec, std::move(task));
    REQUIRE(pumpUntil(exec, [&] { return seen->reachedAwait.load(); }));

    stop.request_stop();
    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->cancelled);

    promise.reject(std::make_exception_ptr(std::runtime_error{"settled after the stop"}));
    exec.runFor(std::chrono::milliseconds{20});
    REQUIRE(seen->error.empty());
}

TEST_CASE("co_await outside any resumption context resumes on the completion's executor", "[coroutine][client]") {
    morph::exec::ThreadPoolExecutor callbacks{1};
    auto [completion, promise] = Completion<int>::makeSettleable(&callbacks);
    auto seen = std::make_shared<Observed>();

    // Started by hand rather than spawned, so no resumption context is current.
    auto task = awaitInto(std::move(completion), seen);
    task.handle().resume();
    REQUIRE(seen->reachedAwait.load());
    promise.resolve(7);

    REQUIRE(morph::testing::waitUntil([&] { return seen->finished.load(); }));
    // One thread: once this task has run, the resumption before it has
    // returned, and the frame may be destroyed.
    std::atomic<bool> drained{false};
    callbacks.post([&] { drained = true; });
    REQUIRE(morph::testing::waitUntil([&] { return drained.load(); }));
    REQUIRE(seen->value == 7);
    REQUIRE(seen->resumedOn != std::this_thread::get_id());
}

TEST_CASE("a stop outside any resumption context resumes the await on the completion's executor",
          "[coroutine][client]") {
    morph::exec::ThreadPoolExecutor callbacks{1};
    auto [completion, promise] = Completion<int>::makeSettleable(&callbacks);
    auto seen = std::make_shared<Observed>();

    auto task = awaitInto(std::move(completion), seen);
    // NOLINTNEXTLINE(misc-const-correctness): request_stop() is non-const
    core::async::StopSource stop;
    task.handle().promise().setStopToken(stop.get_token());
    task.handle().resume();
    REQUIRE(seen->reachedAwait.load());

    // Requested here; the resumption is posted to the completion's executor,
    // never run on the requesting thread.
    stop.request_stop();
    REQUIRE(morph::testing::waitUntil([&] { return seen->finished.load(); }));
    std::atomic<bool> drained{false};
    callbacks.post([&] { drained = true; });
    REQUIRE(morph::testing::waitUntil([&] { return drained.load(); }));
    REQUIRE(seen->cancelled);
    REQUIRE(seen->resumedOn != std::this_thread::get_id());
}

namespace {

/// An executor that refuses every task, as a full queue would.
class RefusingExecutor : public morph::exec::IExecutor {
public:
    void post(std::function<void()> /*task*/) override { throw std::runtime_error{"executor refused the task"}; }
};

}  // namespace

TEST_CASE("co_await whose handler cannot be attached rethrows at the co_await and leaves nothing attached",
          "[coroutine][client]") {
    // Already settled, so attaching fires the handler through the completion's
    // executor at once -- and that executor refuses it, inside await_suspend.
    RefusingExecutor refusing;
    auto [completion, promise] = Completion<int>::makeSettleable(&refusing);
    promise.resolve(5);
    morph::exec::MainThreadExecutor exec;
    auto seen = std::make_shared<Observed>();

    morph::async::spawn(exec, awaitInto(std::move(completion), seen));

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->error == "executor refused the task");
    REQUIRE_FALSE(seen->value.has_value());
    REQUIRE_FALSE(seen->cancelled);
}
