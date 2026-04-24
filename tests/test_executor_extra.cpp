// SPDX-License-Identifier: Apache-2.0

#include <morph/executor.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>


TEST_CASE("morph::exec::MainThreadExecutor: callback that throws is swallowed and loop continues", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};

    exec.post([] { throw std::runtime_error("boom"); });
    exec.post([&] { count.store(1); });

    exec.runFor(std::chrono::milliseconds(200));
    REQUIRE(count.load() == 1);
}

TEST_CASE("morph::exec::MainThreadExecutor: tasks posted before runFor are all drained", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    constexpr int numTasks = 10;
    std::atomic<int> count{0};

    for (int i = 0; i < numTasks; ++i) {
        exec.post([&] { count.fetch_add(1); });
    }

    exec.runFor(std::chrono::milliseconds(500));
    REQUIRE(count.load() == numTasks);
}

TEST_CASE("morph::exec::ThreadPoolExecutor: stop with queued tasks does not deadlock", "[executor]") {
    // Post many tasks and immediately destroy the pool — destructor must join cleanly.
    // Some tasks may not run; the important invariant is no deadlock/crash.
    std::atomic<int> ran{0};
    {
        morph::exec::ThreadPoolExecutor pool{1};
        for (int i = 0; i < 100; ++i) {
            pool.post([&] {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                ran.fetch_add(1);
            });
        }
        // pool destructor joins here — workers drain remaining tasks before exit
    }
    // After join, whatever ran is fine; the test verifies it did not hang.
    REQUIRE(ran.load() >= 0);
}

TEST_CASE("morph::exec::ThreadPoolExecutor: exception in one task does not kill worker", "[executor]") {
    morph::exec::ThreadPoolExecutor pool{1};
    std::atomic<int> afterCount{0};
    constexpr int afterTasks = 5;

    pool.post([] { throw std::runtime_error("worker killer?"); });
    for (int i = 0; i < afterTasks; ++i) {
        pool.post([&] { afterCount.fetch_add(1); });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(afterCount.load() == afterTasks);
}
