// SPDX-License-Identifier: Apache-2.0

#include <morph/executor.hpp>
#include <morph/strand.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <vector>

// Regression test for the per-key serialisation race in StrandExecutor.
//
// The bug: post() captured the strand under _mapMtx, released _mapMtx, then
// re-armed the strand under only strand->mtx. A concurrent drain in
// scheduleNext (holding {_mapMtx, strand->mtx}) could see the pending queue
// empty, clear running, and erase the strand from the map in the window after
// post() released _mapMtx but before it re-armed. The re-armed strand was then
// orphaned: a later post(key) created a *second* strand for the same key, and
// both strands dispatched tasks for that key concurrently — breaking the
// per-model serialisation guarantee.
//
// The test hammers post() on a single key from many threads. Every task bumps a
// per-key in-flight counter on entry and drops it on exit; if two tasks for the
// same key ever run concurrently the counter exceeds 1 and the test fails. Very
// short tasks maximise the drain/re-arm interleaving that triggered the bug.
TEST_CASE("StrandExecutor never runs two tasks for one key concurrently under contention",
          "[strand][race]") {
    constexpr int kThreads = 8;
    constexpr int kPostsPerThread = 400;
    constexpr int kIterations = 20;

    morph::exec::detail::ModelId const key{42};

    for (int iter = 0; iter < kIterations; ++iter) {
        morph::exec::ThreadPoolExecutor pool{4};
        morph::exec::detail::StrandExecutor strand{pool};

        std::atomic<int> inFlight{0};
        std::atomic<int> maxInFlight{0};
        std::atomic<int> completed{0};

        auto task = [&] {
            int const cur = inFlight.fetch_add(1) + 1;
            int prev = maxInFlight.load();
            while (cur > prev && !maxInFlight.compare_exchange_weak(prev, cur)) {
            }
            // Tiny window so drains and re-arms interleave heavily.
            std::this_thread::yield();
            inFlight.fetch_sub(1);
            completed.fetch_add(1);
        };

        std::vector<std::thread> producers;
        producers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            producers.emplace_back([&] {
                for (int i = 0; i < kPostsPerThread; ++i) {
                    strand.post(key, task);
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }

        // Drain: wait for every posted task to complete.
        constexpr int kExpected = kThreads * kPostsPerThread;
        for (int i = 0; i < 2000 && completed.load() < kExpected; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        REQUIRE(completed.load() == kExpected);
        // The core invariant: at most one task for this key ever runs at once.
        REQUIRE(maxInFlight.load() == 1);
    }
}

// Regression test for ThreadPoolExecutor(0): a zero-worker pool used to accept
// tasks that could never run, hanging every post() forever. The constructor now
// clamps the worker count to at least 1, so a pool built with 0 is still usable.
TEST_CASE("ThreadPoolExecutor(0) yields a usable pool", "[executor][race]") {
    morph::exec::ThreadPoolExecutor pool{0};

    std::atomic<bool> ran{false};
    pool.post([&] { ran.store(true); });

    for (int i = 0; i < 1000 && !ran.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(ran.load());
}
