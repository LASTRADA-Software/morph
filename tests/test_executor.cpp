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

TEST_CASE("morph::exec::MainThreadExecutor runOnce runs exactly one queued task", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};
    exec.post([&] { count.fetch_add(1); });
    exec.post([&] { count.fetch_add(1); });

    bool const ranFirst = exec.runOnce();
    REQUIRE(ranFirst);
    REQUIRE(count.load() == 1);

    bool const ranSecond = exec.runOnce();
    REQUIRE(ranSecond);
    REQUIRE(count.load() == 2);
}

TEST_CASE("morph::exec::MainThreadExecutor runOnce returns immediately and false on an empty queue", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    auto const startTime = std::chrono::steady_clock::now();
    bool const ran = exec.runOnce();
    auto const elapsed = std::chrono::steady_clock::now() - startTime;
    REQUIRE_FALSE(ran);
    // Must not block waiting for a task — this is the whole point of runOnce().
    REQUIRE(elapsed < std::chrono::milliseconds(50));
}

TEST_CASE("morph::exec::MainThreadExecutor runOnce logs and swallows std::exception, still consuming the task",
          "[executor]") {
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};
    exec.post([] { throw std::runtime_error("boom"); });
    exec.post([&] { count.fetch_add(1); });

    REQUIRE(exec.runOnce());  // consumes the throwing task
    REQUIRE(count.load() == 0);
    REQUIRE(exec.runOnce());  // consumes the second task
    REQUIRE(count.load() == 1);
}

TEST_CASE("morph::exec::MainThreadExecutor drain runs all queued tasks without blocking for new ones", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    constexpr int numTasks = 25;
    std::atomic<int> count{0};
    for (int i = 0; i < numTasks; ++i) {
        exec.post([&] { count.fetch_add(1); });
    }

    auto const startTime = std::chrono::steady_clock::now();
    exec.drain();
    auto const elapsed = std::chrono::steady_clock::now() - startTime;

    REQUIRE(count.load() == numTasks);
    // Must return as soon as the queue empties, not wait around for a timeout.
    REQUIRE(elapsed < std::chrono::milliseconds(200));
}

TEST_CASE("morph::exec::MainThreadExecutor drain returns immediately when the queue is already empty", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    auto const startTime = std::chrono::steady_clock::now();
    exec.drain();
    auto const elapsed = std::chrono::steady_clock::now() - startTime;
    REQUIRE(elapsed < std::chrono::milliseconds(50));
}

TEST_CASE("morph::exec::MainThreadExecutor drain runs a bounded chain of tasks that each repost once more",
          "[executor]") {
    // A task that posts one more piece of work while running must not make
    // drain() spin forever, but the freshly-posted task lands back in the
    // (still non-empty) queue and must still run before drain() returns.
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};
    constexpr int chainLength = 3;

    std::function<void(int)> postChain = [&](int remaining) {
        if (remaining <= 0) {
            return;
        }
        count.fetch_add(1);
        exec.post([&postChain, remaining] { postChain(remaining - 1); });
    };
    exec.post([&] { postChain(chainLength); });

    exec.drain();
    REQUIRE(count.load() == chainLength);
}
