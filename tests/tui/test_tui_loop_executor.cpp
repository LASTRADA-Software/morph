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
    CHECK(logged.front().contains("boom"));
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
