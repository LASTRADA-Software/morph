// SPDX-License-Identifier: Apache-2.0

#include <morph/strand.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <vector>


TEST_CASE("morph::exec::detail::StrandExecutor serialises tasks for the same key", "[strand]") {
    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::detail::StrandExecutor strand{pool};
    morph::exec::detail::ModelId key{1};

    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> completed{0};
    constexpr int numTasks = 20;

    for (int i = 0; i < numTasks; ++i) {
        strand.post(key, [&] {
            int cnt = concurrent.fetch_add(1) + 1;
            int prev = maxConcurrent.load();
            while (cnt > prev && !maxConcurrent.compare_exchange_weak(prev, cnt)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            concurrent.fetch_sub(1);
            completed.fetch_add(1);
        });
    }

    // Wait for all tasks to finish
    for (int i = 0; i < 100 && completed.load() < numTasks; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(completed.load() == numTasks);
    REQUIRE(maxConcurrent.load() == 1);  // never more than 1 at a time for same key
}

TEST_CASE("morph::exec::detail::StrandExecutor runs tasks for different keys concurrently", "[strand]") {
    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::detail::StrandExecutor strand{pool};

    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> completed{0};
    constexpr int numKeys = 4;

    for (int i = 0; i < numKeys; ++i) {
        strand.post(morph::exec::detail::ModelId{static_cast<uint64_t>(i + 1)}, [&] {
            int cnt = concurrent.fetch_add(1) + 1;
            int prev = maxConcurrent.load();
            while (cnt > prev && !maxConcurrent.compare_exchange_weak(prev, cnt)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            concurrent.fetch_sub(1);
            completed.fetch_add(1);
        });
    }

    for (int i = 0; i < 50 && completed.load() < numKeys; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(completed.load() == numKeys);
    REQUIRE(maxConcurrent.load() > 1);  // different keys ran in parallel
}
