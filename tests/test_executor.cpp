// SPDX-License-Identifier: Apache-2.0

#include <morph/core/executor.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>


TEST_CASE("morph::exec::ThreadPoolExecutor runs posted tasks", "[executor]") {
    morph::exec::ThreadPoolExecutor pool{2};
    std::atomic<int> count{0};
    for (int i = 0; i < 10; ++i) {
        pool.post([&] { count.fetch_add(1); });
    }
    // Give workers time to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(count.load() == 10);
}

TEST_CASE("morph::exec::ThreadPoolExecutor swallows exceptions from tasks", "[executor]") {
    morph::exec::ThreadPoolExecutor pool{1};
    std::atomic<bool> reached{false};
    pool.post([] { throw std::runtime_error("boom"); });
    pool.post([&] { reached.store(true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(reached.load());
}

TEST_CASE("morph::exec::MainThreadExecutor drains tasks via runFor", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};
    for (int i = 0; i < 5; ++i) {
        exec.post([&] { count.fetch_add(1); });
    }
    exec.runFor(std::chrono::milliseconds(200));
    REQUIRE(count.load() == 5);
}

TEST_CASE("morph::exec::MainThreadExecutor times out when queue is empty", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    auto startTime = std::chrono::steady_clock::now();
    exec.runFor(std::chrono::milliseconds(50));
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    // Should return roughly on time (within 2x)
    REQUIRE(elapsed < std::chrono::milliseconds(200));
}
