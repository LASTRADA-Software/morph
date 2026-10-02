// SPDX-License-Identifier: Apache-2.0
//
// morph::exec::IoLoop, the loop every socket, timer and probe of an
// application shares: docs/spec/concurrency_and_lifetimes.md, "Thread roles".

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <stdexcept>
#include <thread>

#include "test_support.hpp"

namespace {

bool onLoop(morph::exec::IoLoop& loop) {
    return morph::exec::runningOn(static_cast<core::async::IExecutor const&>(loop.loop()));
}

}  // namespace

TEST_CASE("IoLoop: a posted task runs as a task of the loop, on another thread", "[io_loop]") {
    morph::exec::IoLoop loop;
    REQUIRE_FALSE(loop.runningHere());
    REQUIRE_FALSE(onLoop(loop));

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
    REQUIRE(morph::testing::waitUntil([&] { return ran.load(); }));
    CHECK(runningHere.load());
    CHECK(scoped.load());
    CHECK(where != std::this_thread::get_id());
}

TEST_CASE("IoLoop: runAndWait returns once the task has run, and runs inline on the loop", "[io_loop]") {
    morph::exec::IoLoop loop;
    bool ranOnLoop = false;
    loop.runAndWait([&] { ranOnLoop = onLoop(loop); });
    CHECK(ranOnLoop);

    // From inside a task of the loop, waiting on the loop would wait forever:
    // it runs the nested task inline instead.
    bool nestedRan = false;
    loop.runAndWait([&] { loop.runAndWait([&] { nestedRan = onLoop(loop); }); });
    CHECK(nestedRan);
}

TEST_CASE("IoLoop: a task that throws is logged and later tasks still run", "[io_loop]") {
    morph::exec::IoLoop loop;
    loop.post([] { throw std::runtime_error{"boom"}; });
    bool later = false;
    loop.runAndWait([&] { later = true; });
    CHECK(later);
}

TEST_CASE("IoLoop: a weak handle stops posting once the loop is gone", "[io_loop]") {
    auto loop = std::make_unique<morph::exec::IoLoop>();
    auto const weak = loop->weak();
    std::atomic<bool> ran{false};
    REQUIRE(weak.post([&] { ran = true; }));
    REQUIRE(morph::testing::waitUntil([&] { return ran.load(); }));
    loop.reset();
    CHECK_FALSE(weak.post([] {}));
}

TEST_CASE("IoLoop: destroyed from one of its own tasks, it lets the thread finish instead of joining it",
          "[io_loop]") {
    auto loop = std::make_unique<morph::exec::IoLoop>();
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
