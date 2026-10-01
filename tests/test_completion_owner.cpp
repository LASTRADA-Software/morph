// SPDX-License-Identifier: Apache-2.0
//
// A Completion belongs to its executor: a settle from any thread claims the
// result once and posts its delivery there, and handlers are attached and run
// there. See docs/spec/core/completion.md, "Thread safety".

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <core/async/Task.hpp>
#include <latch>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace {

using morph::async::Completion;

/// Runs every task queued on @p exec, without waiting for more; returns how many ran.
int drainCounting(morph::exec::MainThreadExecutor& exec) {
    int ran = 0;
    while (exec.runOnce()) {
        ++ran;
    }
    return ran;
}

/// Runs @p work as a task of @p pool and waits for it.
template <typename F>
void onPool(morph::exec::ThreadPoolExecutor& pool, F work) {
    std::atomic<bool> done{false};
    pool.post([&] {
        work();
        done = true;
    });
    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
}

}  // namespace

TEST_CASE("Completion: a settle from a pool thread with no handler attached posts its delivery to the owner",
          "[completion][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{1};
    auto [completion, promise] = Completion<int>::makeSettleable(&owner);

    onPool(pool, [&promise] { promise.resolve(17); });

    // The settle posted its delivery although nothing was attached yet: the
    // owner applies it in a task of its own.
    REQUIRE(drainCounting(owner) == 1);

    // A later attach, on the owner, observes the delivered value there.
    std::optional<int> seen;
    bool ranOnOwner = false;
    owner.post([&] {
        completion.then([&](int value) {
            seen = value;
            ranOnOwner = morph::exec::runningOn(owner);
        });
    });
    drainCounting(owner);
    REQUIRE(seen == 17);
    REQUIRE(ranOnOwner);
}

TEST_CASE("Completion: a handler attached before a pool settle runs inside a task of the owner",
          "[completion][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{1};
    auto [completion, promise] = Completion<int>::makeSettleable(&owner);

    std::optional<int> seen;
    bool ranOnOwner = false;
    std::thread::id ranOn;
    completion.then([&](int value) {
        seen = value;
        ranOnOwner = morph::exec::runningOn(owner);
        ranOn = std::this_thread::get_id();
    });

    onPool(pool, [&promise] { promise.resolve(5); });
    REQUIRE_FALSE(seen.has_value());

    REQUIRE(drainCounting(owner) == 1);
    REQUIRE(seen == 5);
    REQUIRE(ranOnOwner);
    REQUIRE(ranOn == std::this_thread::get_id());
}

TEST_CASE("Completion: attaching from another executor's task is reported as off the owner", "[completion][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::ThreadPoolExecutor pool{1};
    auto [completion, promise] = Completion<int>::makeSettleable(&owner);
    morph::testing::OwnerProbeRecorder const recorder{owner.coreExecutor()};

    onPool(pool, [&] { completion.then([](int) {}); });

    auto const seen = recorder.at("Completion::attach");
    REQUIRE(seen.size() == 1);
    CHECK(seen.front().expectedOwner);
    CHECK_FALSE(seen.front().onOwner);
}

TEST_CASE("Completion: an attach on a thread outside every task is checked against where delivery runs",
          "[completion][owner]") {
    SECTION("the owner's own thread: the presumption holds and nothing is reported") {
        morph::exec::MainThreadExecutor owner;
        auto [completion, promise] = Completion<int>::makeSettleable(&owner);
        morph::testing::OwnerProbeRecorder const recorder{owner.coreExecutor()};

        bool fired = false;
        completion.then([&](int) { fired = true; });
        std::thread{[p = std::move(promise)]() mutable { p.resolve(1); }}.join();
        drainCounting(owner);
        REQUIRE(fired);
        CHECK(recorder.count("Completion::deliver") == 0);
        CHECK(recorder.count("Completion::attach") == 0);
    }
    SECTION("a pool's completion: delivery on a pool thread refutes it, and is reported") {
        morph::exec::ThreadPoolExecutor pool{1};
        auto [completion, promise] = Completion<int>::makeSettleable(&pool);
        morph::testing::OwnerProbeRecorder const recorder{pool.coreExecutor()};

        std::atomic<bool> fired{false};
        completion.then([&](int) { fired = true; });
        promise.resolve(1);
        REQUIRE(morph::testing::waitUntil([&] { return fired.load(); }));
        CHECK(recorder.count("Completion::deliver") == 1);
    }
}

TEST_CASE("Completion: two settles racing from two threads deliver exactly once", "[completion][owner]") {
    morph::exec::MainThreadExecutor owner;
    auto [completion, promise] = Completion<int>::makeSettleable(&owner);
    auto state = completion.state();

    std::vector<int> seen;
    completion.then([&](int value) { seen.push_back(value); });

    for (int round = 0; round < 50; ++round) {
        auto [racing, racingPromise] = Completion<int>::makeSettleable(&owner);
        std::vector<int> racingSeen;
        racing.then([&](int value) { racingSeen.push_back(value); });
        std::latch start{2};
        std::thread first{[&, settle = racing.state()] {
            start.arrive_and_wait();
            settle->setValue(1);
        }};
        std::thread second{[&, settle = racing.state()] {
            start.arrive_and_wait();
            settle->setValue(2);
        }};
        first.join();
        second.join();
        REQUIRE(drainCounting(owner) == 1);
        REQUIRE(racingSeen.size() == 1);
    }

    // The first settle wins, and a later one is dropped before it posts.
    std::thread{[&] { state->setValue(10); }}.join();
    std::thread{[&] { state->setValue(20); }}.join();
    REQUIRE(drainCounting(owner) == 1);
    REQUIRE(seen == std::vector<int>{10});

    std::optional<int> late;
    completion.then([&](int value) { late = value; });
    drainCounting(owner);
    REQUIRE(late == 10);
}

TEST_CASE("Completion: an error settled with nothing attached is logged as an orphan once, after its delivery",
          "[completion][owner]") {
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride const guard{
        [&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); },
        morph::log::LogLevel::debug,
    };
    morph::exec::MainThreadExecutor owner;
    {
        auto [completion, promise] = Completion<int>::makeSettleable(&owner);
        std::thread{[p = std::move(promise)]() mutable {
            p.reject(std::make_exception_ptr(std::runtime_error{"nobody listens"}));
        }}.join();
    }
    // Both handles are gone; the queued delivery still holds the state.
    REQUIRE(logged.empty());
    drainCounting(owner);
    REQUIRE(logged.size() == 1);
    REQUIRE(logged.front().find("nobody listens") != std::string::npos);
}

namespace {

struct AwaitSeen {
    std::optional<int> value;
    std::string error;
    bool resumedOnAwaiter = false;
    std::atomic<bool> finished{false};
};

core::async::Task<void> awaitOn(Completion<int> completion, std::shared_ptr<AwaitSeen> seen,
                                morph::exec::MainThreadExecutor& awaiter) {
    try {
        seen->value = co_await std::move(completion);
    } catch (const std::exception& exc) {
        seen->error = exc.what();
    }
    seen->resumedOnAwaiter = morph::exec::runningOn(awaiter);
    seen->finished = true;
}

}  // namespace

TEST_CASE("Completion: co_await from a coroutine on another executor posts its attach to the owner and resumes",
          "[completion][owner][coroutine]") {
    morph::exec::MainThreadExecutor owner;
    morph::exec::MainThreadExecutor awaiter;
    auto [completion, promise] = Completion<int>::makeSettleable(&owner);
    morph::testing::OwnerProbeRecorder const recorder{owner.coreExecutor()};
    auto seen = std::make_shared<AwaitSeen>();

    morph::async::spawn(awaiter, awaitOn(std::move(completion), seen, awaiter));
    drainCounting(awaiter);  // runs to the co_await and suspends
    REQUIRE_FALSE(seen->finished.load());

    std::thread{[p = std::move(promise)]() mutable { p.resolve(23); }}.join();

    // Nothing resumes until the owner has run the posted attach and the
    // delivery: both are the owner's.
    drainCounting(awaiter);
    REQUIRE_FALSE(seen->finished.load());
    REQUIRE(drainCounting(owner) == 2);

    drainCounting(awaiter);
    REQUIRE(seen->finished.load());
    CHECK(seen->value == 23);
    CHECK(seen->error.empty());
    CHECK(seen->resumedOnAwaiter);
    // The attach was not made from the awaiter's task.
    CHECK(recorder.count("Completion::attach") == 0);
}
