// SPDX-License-Identifier: Apache-2.0
//
// The executor context of docs/spec/core/executor.md, "Current executor": a
// task run by ThreadPoolExecutor or MainThreadExecutor states a
// core::async::ExecutorScope naming that executor, and `runningOn()` reads it.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/ExecutorContext.hpp>
#include <cstddef>
#include <future>
#include <morph/core/executor.hpp>
#include <optional>
#include <stdexcept>

namespace {

using morph::exec::MainThreadExecutor;
using morph::exec::runningOn;
using morph::exec::ThreadPoolExecutor;

/// What one task observed about the executor context it ran in.
struct Seen {
    bool onFirst{false};
    bool onSecond{false};
    bool inlineOnFirst{false};
    bool currentIsFirst{false};
    std::size_t depth{0};
};

/// How many scopes are in force on the calling thread.
std::size_t scopeDepth() {
    std::size_t depth = 0;
    for (auto const* scope = core::async::ExecutorScope::innermost(); scope != nullptr; scope = scope->previous()) {
        ++depth;
    }
    return depth;
}

/// Observes the calling thread's context from inside a task of @p first.
Seen observe(morph::exec::IExecutor& first, morph::exec::IExecutor* second = nullptr) {
    Seen seen;
    seen.onFirst = runningOn(first);
    seen.onSecond = second != nullptr && runningOn(*second);
    seen.currentIsFirst = core::async::currentExecutor() == &first.coreExecutor();
    seen.depth = scopeDepth();
    morph::exec::detail::inlineExecutor().post([&] { seen.inlineOnFirst = runningOn(first); });
    return seen;
}

/// Runs @p body as a task of @p pool and returns what it produced.
template <typename Body>
Seen runOnPool(ThreadPoolExecutor& pool, Body body) {
    std::promise<Seen> done;
    auto result = done.get_future();
    pool.post([&] { done.set_value(body()); });
    REQUIRE(result.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    return result.get();
}

/// Runs @p body as the one task of @p exec, pumped on this thread.
template <typename Body>
Seen runOnMain(MainThreadExecutor& exec, Body body) {
    std::optional<Seen> seen;
    exec.post([&] { seen = body(); });
    REQUIRE(exec.runOnce());
    REQUIRE(seen.has_value());
    return *seen;
}

}  // namespace

TEST_CASE("outside any task no executor is current", "[executor][scope]") {
    ThreadPoolExecutor pool{1};
    MainThreadExecutor mainExec;

    CHECK_FALSE(runningOn(pool));
    CHECK_FALSE(runningOn(mainExec));
    CHECK(core::async::currentExecutor() == nullptr);
}

TEST_CASE("a ThreadPoolExecutor task runs inside a scope naming the pool", "[executor][scope]") {
    ThreadPoolExecutor pool{2};

    auto const seen = runOnPool(pool, [&] { return observe(pool); });

    CHECK(seen.onFirst);
    CHECK(seen.currentIsFirst);
}

TEST_CASE("a MainThreadExecutor task runs inside a scope naming the executor", "[executor][scope]") {
    MainThreadExecutor mainExec;

    auto const seen = runOnMain(mainExec, [&] { return observe(mainExec); });

    CHECK(seen.onFirst);
    CHECK(seen.currentIsFirst);
}

TEST_CASE("an inline task inside a ThreadPoolExecutor task is still on the pool", "[executor][scope]") {
    ThreadPoolExecutor pool{1};

    auto const seen = runOnPool(pool, [&] { return observe(pool); });

    CHECK(seen.inlineOnFirst);
}

TEST_CASE("an inline task inside a MainThreadExecutor task is still on that executor", "[executor][scope]") {
    MainThreadExecutor mainExec;

    auto const seen = runOnMain(mainExec, [&] { return observe(mainExec); });

    CHECK(seen.inlineOnFirst);
}

TEST_CASE("a ThreadPoolExecutor task is not running on another executor", "[executor][scope]") {
    ThreadPoolExecutor first{1};
    ThreadPoolExecutor second{1};
    MainThreadExecutor mainExec;

    auto const seen = runOnPool(first, [&] { return observe(first, &second); });
    REQUIRE(seen.onFirst);
    CHECK_FALSE(seen.onSecond);

    auto const onMain = runOnPool(first, [&] { return observe(first, &mainExec); });
    REQUIRE(onMain.onFirst);
    CHECK_FALSE(onMain.onSecond);
}

TEST_CASE("a MainThreadExecutor task is not running on another executor", "[executor][scope]") {
    MainThreadExecutor first;
    ThreadPoolExecutor pool{1};

    auto const seen = runOnMain(first, [&] { return observe(first, &pool); });

    REQUIRE(seen.onFirst);
    CHECK_FALSE(seen.onSecond);
}

TEST_CASE("a ThreadPoolExecutor task starts with only its own scope in force", "[executor][scope]") {
    // One worker, so the second task runs on the thread that ran the first: a
    // scope the first task left behind would show as a second level.
    ThreadPoolExecutor pool{1};

    auto const first = runOnPool(pool, [&] { return observe(pool); });
    auto const second = runOnPool(pool, [&] { return observe(pool); });

    CHECK(first.depth == 1);
    CHECK(second.depth == 1);
}

TEST_CASE("a MainThreadExecutor leaves no scope on the thread once a task returns", "[executor][scope]") {
    MainThreadExecutor mainExec;

    auto const seen = runOnMain(mainExec, [&] { return observe(mainExec); });

    CHECK(seen.depth == 1);
    CHECK_FALSE(runningOn(mainExec));
    CHECK(core::async::currentExecutor() == nullptr);
}

TEST_CASE("a MainThreadExecutor task that throws still leaves no scope on the thread", "[executor][scope]") {
    MainThreadExecutor mainExec;
    mainExec.post([] { throw std::runtime_error{"thrown on purpose"}; });

    REQUIRE(mainExec.runOnce());

    CHECK_FALSE(runningOn(mainExec));
    CHECK(core::async::currentExecutor() == nullptr);
}

TEST_CASE("coreExecutor is one stable identity per executor", "[executor][scope]") {
    ThreadPoolExecutor pool{1};
    MainThreadExecutor mainExec;

    CHECK(&pool.coreExecutor() == &pool.coreExecutor());
    CHECK(&mainExec.coreExecutor() == &mainExec.coreExecutor());
    CHECK(&pool.coreExecutor() != &mainExec.coreExecutor());
}
